#pragma once
#include "rfmodel/core/device_model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cfenv>
#include <limits>
#include <vector>

namespace rfmodel {
namespace tabulated_amplifier_detail {
using Wide = long double;

// Carry binary exponents separately, so a small quotient can be multiplied by
// a large factor before rounding the final, potentially representable result.
inline Wide scaled(std::initializer_list<Wide> numerator,
                   std::initializer_list<Wide> denominator = {}) {
    Wide mantissa = 1;
    int exponent = 0;
    for (Wide factor : numerator) {
        if (factor == 0) {
            return 0;
        }
        int e = 0;
        mantissa *= std::frexp(factor, &e);
        exponent += e;
    }
    for (Wide factor : denominator) {
        int e = 0;
        mantissa /= std::frexp(factor, &e);
        exponent -= e;
    }
    return std::scalbn(mantissa, exponent);
}

inline Wide term(Wide coefficient,
                 Wide distance,
                 Wide width,
                 unsigned power,
                 unsigned denominator_power,
                 Wide multiplier = 1) {
    Wide mantissa = 1;
    int exponent = 0;
    auto factor = [&](Wide value, bool divide) {
        int e = 0;
        const Wide m = std::frexp(value, &e);
        if (divide) {
            mantissa /= m;
            exponent -= e;
        } else {
            mantissa *= m;
            exponent += e;
        }
    };
    if (coefficient == 0 || multiplier == 0 || (power && distance == 0)) {
        return 0;
    }
    factor(coefficient, false);
    factor(multiplier, false);
    for (unsigned i = 0; i < power; ++i) {
        factor(distance, false);
    }
    for (unsigned i = 0; i < denominator_power; ++i) {
        factor(width, true);
    }
    return std::scalbn(mantissa, exponent);
}
} // namespace tabulated_amplifier_detail

struct TabulatedAmplifierNode {
    double radius_sqrt_w, amplitude_sqrt_w, phase_rad, amplitude_slope, phase_slope;
};

struct TabulatedAmplifierResponse {
    double amplitude_sqrt_w, phase_rad, amplitude_slope, phase_slope;
    Complex gain, radial_gain_difference;
};

// Native, explicitly unwrapped Hermite table. No extrapolation or slope fitting.
class TabulatedAmplifier {
    using Wide = tabulated_amplifier_detail::Wide;
    using Polynomial = std::array<Wide, 4>;

    struct Segment {
        Wide width;
        Polynomial amplitude, phase, reverse_amplitude, reverse_phase;
    };

    std::vector<TabulatedAmplifierNode> nodes_;
    std::vector<Segment> segments_;

    static double narrow(Wide value) {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<double>::max()) {
            throw std::overflow_error("tabulated amplifier result exceeds finite double range");
        }
        return static_cast<double>(value);
    }

    static Polynomial polynomial(Wide a, Wide b, Wide da, Wide db, Wide width) {
        const Wide m = da * width, n = db * width, delta = b - a;
        if (!std::isfinite(width) || (da != 0 && m == 0) || (db != 0 && n == 0)) {
            throw std::overflow_error(
                "tabulated amplifier slope interval product is unrepresentable");
        }
        Polynomial result{a, m, 3 * delta - 2 * m - n, -2 * delta + m + n};
        for (auto value : result) {
            if (!std::isfinite(value)) {
                throw std::overflow_error("tabulated amplifier Hermite coefficients overflow");
            }
        }
        return result;
    }

    static Wide value(const Polynomial &p, Wide distance, Wide width) {
        using tabulated_amplifier_detail::term;
        return p[0] + term(p[1], distance, width, 1, 1) + term(p[2], distance, width, 2, 2) +
               term(p[3], distance, width, 3, 3);
    }

    static Wide slope(const Polynomial &p, Wide distance, Wide width) {
        using tabulated_amplifier_detail::term;
        return term(p[1], distance, width, 0, 1) + term(2 * p[2], distance, width, 1, 2) +
               term(3 * p[3], distance, width, 2, 3);
    }

    struct Interval {
        Wide low, high;
    };

    static Interval enclosure(Wide low, Wide high) {
        if (!std::isfinite(low) || !std::isfinite(high)) {
            throw std::overflow_error("tabulated amplifier positivity arithmetic overflow");
        }
        const Wide lower = std::nextafter(low, -std::numeric_limits<Wide>::infinity());
        const Wide upper = std::nextafter(high, std::numeric_limits<Wide>::infinity());
        if (!std::isfinite(lower) || !std::isfinite(upper)) {
            throw std::overflow_error("tabulated amplifier positivity bounds overflow");
        }
        return {lower, upper};
    }

    static Interval sum(Wide a, Wide b) {
        const Wide s = a + b;
        if (!std::isfinite(s)) {
            return enclosure(s, s);
        }
        const Wide v = s - a;
        const Wide error = (a - (s - v)) + (b - v); // error-free TwoSum
        if (!std::isfinite(v) || !std::isfinite(error)) {
            return enclosure(s, s);
        }
        if (error == 0) {
            return {s, s};
        }
        const auto bounds = enclosure(s, s);
        return error < 0 ? Interval{bounds.low, s} : Interval{s, bounds.high};
    }

    static Interval product(Wide a, Wide b) {
        const Wide p = a * b;
        if (a == 0 || b == 0) {
            return {0, 0};
        }
        if (!std::isfinite(p)) {
            return enclosure(p, p);
        }
        const Wide error = std::fma(a, b, -p);
        // In the underflow region the exact FMA residual can round to zero.
        if (std::abs(p) <
            std::ldexp(std::numeric_limits<Wide>::min(), std::numeric_limits<Wide>::digits)) {
            return enclosure(p, p);
        }
        if (error == 0) {
            return {p, p};
        }
        const auto bounds = enclosure(p, p);
        return error < 0 ? Interval{bounds.low, p} : Interval{p, bounds.high};
    }

    static Interval add(Interval a, Interval b) {
        if (b.low == 0 && b.high == 0) {
            return a;
        }
        if (a.low == 0 && a.high == 0) {
            return b;
        }
        return {sum(a.low, b.low).low, sum(a.high, b.high).high};
    }

    static Interval multiply(Interval a, Wide b) {
        if (b == 0 || (a.low == 0 && a.high == 0)) {
            return {0, 0};
        }
        return b > 0 ? Interval{product(a.low, b).low, product(a.high, b).high}
                     : Interval{product(a.high, b).low, product(a.low, b).high};
    }

    static Interval midpoint(Interval a, Interval b) {
        if (a.low == 0 && a.high == 0 && b.low == 0 && b.high == 0) {
            return {0, 0};
        }
        // Halve before adding to avoid overflow. Widen each operation separately.
        return add(multiply(a, .5L), multiply(b, .5L));
    }

    static void nonnegative(const TabulatedAmplifierNode &left,
                            const TabulatedAmplifierNode &right) {
        // Bernstein convex hulls with outward-rounded arithmetic are a sufficient
        // certificate of nonnegativity, including between stationary points.
        // Subdivision refines mixed-sign hulls; uncertainty NEVER admits a table.
        const auto width = sum(Wide(right.radius_sqrt_w), -Wide(left.radius_sqrt_w));
        // Use three times the Bernstein controls: no rounded division by three,
        // so exactly representable interior double roots can be certified.
        const Interval first = product(left.amplitude_sqrt_w, 3);
        const Interval last = product(right.amplitude_sqrt_w, 3);
        auto tangent = [&](double derivative) {
            return derivative == 0 ? Interval{0, 0} : multiply(width, derivative);
        };
        using Hull = std::array<Interval, 4>;

        struct Pending {
            Hull hull;
            unsigned depth;
        };

        std::vector<Pending> pending{{{first,
                                       add(first, tangent(left.amplitude_slope)),
                                       add(last, tangent(-right.amplitude_slope)),
                                       last},
                                      0}};
        std::size_t work = 0;
        while (!pending.empty()) {
            const auto item = pending.back();
            pending.pop_back();
            bool certified = true;
            for (auto control : item.hull) {
                certified = certified && control.low >= 0;
            }
            if (certified) {
                continue;
            }
            if (++work > 4096 || item.depth >= 64) {
                throw std::invalid_argument(
                    "tabulated amplifier amplitude nonnegativity cannot be certified");
            }
            const auto &b = item.hull;
            const auto x = midpoint(b[0], b[1]), y = midpoint(b[1], b[2]), z = midpoint(b[2], b[3]);
            const auto u = midpoint(x, y), v = midpoint(y, z), m = midpoint(u, v);
            if (m.high < 0) {
                throw std::invalid_argument(
                    "tabulated amplifier amplitude is negative inside segment");
            }
            pending.push_back({{m, v, z, b[3]}, item.depth + 1});
            pending.push_back({{b[0], x, u, m}, item.depth + 1});
        }
    }

    static Complex rotate(Wide real, Wide imaginary, Wide phase) {
        const Wide c = std::cos(phase), s = std::sin(phase);
        return {narrow(real * c - imaginary * s), narrow(real * s + imaginary * c)};
    }

public:
    explicit TabulatedAmplifier(std::vector<TabulatedAmplifierNode> nodes)
        : nodes_(std::move(nodes)) {
        if (std::fegetround() != FE_TONEAREST) {
            throw std::invalid_argument("tabulated amplifier requires round-to-nearest arithmetic");
        }
        if (nodes_.size() < 2) {
            throw std::invalid_argument("tabulated amplifier needs at least two nodes");
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const auto &n = nodes_[i];
            if (!std::isfinite(n.radius_sqrt_w) || !std::isfinite(n.amplitude_sqrt_w) ||
                !std::isfinite(n.phase_rad) || !std::isfinite(n.amplitude_slope) ||
                !std::isfinite(n.phase_slope) || n.amplitude_sqrt_w < 0 ||
                (i == 0 &&
                 (n.radius_sqrt_w != 0 || n.amplitude_sqrt_w != 0 || n.amplitude_slope < 0)) ||
                (i && n.radius_sqrt_w <= nodes_[i - 1].radius_sqrt_w)) {
                throw std::invalid_argument("invalid tabulated amplifier node");
            }
            if (i) {
                const auto &previous = nodes_[i - 1];
                const Wide width = Wide(n.radius_sqrt_w) - previous.radius_sqrt_w;
                Segment segment{width,
                                polynomial(previous.amplitude_sqrt_w,
                                           n.amplitude_sqrt_w,
                                           previous.amplitude_slope,
                                           n.amplitude_slope,
                                           width),
                                polynomial(previous.phase_rad,
                                           n.phase_rad,
                                           previous.phase_slope,
                                           n.phase_slope,
                                           width),
                                polynomial(n.amplitude_sqrt_w,
                                           previous.amplitude_sqrt_w,
                                           -n.amplitude_slope,
                                           -previous.amplitude_slope,
                                           width),
                                polynomial(n.phase_rad,
                                           previous.phase_rad,
                                           -n.phase_slope,
                                           -previous.phase_slope,
                                           width)};
                if ((previous.amplitude_sqrt_w == 0 && previous.amplitude_slope < 0) ||
                    (n.amplitude_sqrt_w == 0 && n.amplitude_slope > 0)) {
                    throw std::invalid_argument(
                        "tabulated amplifier zero endpoint has negative adjacent amplitude");
                }
                nonnegative(previous, n);
                segments_.push_back(segment);
            }
        }
    }

    const std::vector<TabulatedAmplifierNode> &nodes() const noexcept {
        return nodes_;
    }

private:
    struct Evaluation {
        Wide amplitude, phase, derivative, phase_derivative, gain, radial, amplitude_phase_slope;
    };

    Evaluation evaluate(double radius) const {
        if (std::fegetround() != FE_TONEAREST) {
            throw std::invalid_argument("tabulated amplifier requires round-to-nearest arithmetic");
        }
        if (!std::isfinite(radius) || radius < 0) {
            throw std::invalid_argument("invalid tabulated amplifier radius");
        }
        if (radius > nodes_.back().radius_sqrt_w) {
            throw std::out_of_range("tabulated amplifier radius exceeds table");
        }
        const auto upper = std::upper_bound(
            nodes_.begin(), nodes_.end(), radius, [](double r, const TabulatedAmplifierNode &n) {
                return r < n.radius_sqrt_w;
            });
        const auto index =
            std::min(static_cast<std::size_t>(upper - nodes_.begin() - 1), segments_.size() - 1);
        const auto &segment = segments_[index];
        const Wide t = (Wide(radius) - nodes_[index].radius_sqrt_w) / segment.width;
        const bool reverse = t > .5L;
        const Wide local = reverse ? Wide(nodes_[index + 1].radius_sqrt_w) - radius
                                   : Wide(radius) - nodes_[index].radius_sqrt_w;
        const auto &ap = reverse ? segment.reverse_amplitude : segment.amplitude;
        const auto &pp = reverse ? segment.reverse_phase : segment.phase;
        Wide amplitude = value(ap, local, segment.width), phase = value(pp, local, segment.width);
        Wide derivative = (reverse ? -1 : 1) * slope(ap, local, segment.width);
        Wide phase_derivative = (reverse ? -1 : 1) * slope(pp, local, segment.width);
        using tabulated_amplifier_detail::term;
        // Multiply the amplitude and phase-derivative polynomials before final
        // rounding: either factor alone can underflow while their product fits.
        Wide amplitude_phase_slope = 0;
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned j = 1; j < 4; ++j) {
                amplitude_phase_slope += term(ap[i],
                                              local,
                                              segment.width,
                                              i + j - 1,
                                              i + j,
                                              (reverse ? -1 : 1) * Wide(j) * pp[j]);
            }
        }
        // Nodes return the supplied values/slopes exactly, with one-sided endpoint derivatives.
        if (radius == nodes_[index].radius_sqrt_w || radius == nodes_.back().radius_sqrt_w) {
            const auto &node =
                radius == nodes_.back().radius_sqrt_w ? nodes_.back() : nodes_[index];
            amplitude = node.amplitude_sqrt_w;
            phase = node.phase_rad;
            derivative = node.amplitude_slope;
            phase_derivative = node.phase_slope;
            amplitude_phase_slope =
                tabulated_amplifier_detail::scaled({amplitude, phase_derivative});
        }
        Wide gain = radius == 0 ? derivative : amplitude / radius;
        Wide radial = radius == 0 ? 0 : derivative - gain;
        if (index == 0 && !reverse && radius != 0 && radius != nodes_.back().radius_sqrt_w) {
            // Analytic A/r and A'-A/r avoid cancellation and underflow near the origin.
            using tabulated_amplifier_detail::term;
            const auto &a = segment.amplitude;
            gain = term(a[1], radius, segment.width, 0, 1) +
                   term(a[2], radius, segment.width, 1, 2) +
                   term(a[3], radius, segment.width, 2, 3);
            radial = term(a[2], radius, segment.width, 1, 2) +
                     term(2 * a[3], radius, segment.width, 2, 3);
        }
        if (amplitude < 0) {
            throw std::overflow_error(
                "tabulated amplifier amplitude evaluation lost nonnegativity");
        }
        return {
            amplitude, phase, derivative, phase_derivative, gain, radial, amplitude_phase_slope};
    }

public:
    TabulatedAmplifierResponse response(double radius) const {
        const auto r = evaluate(radius);
        return {narrow(r.amplitude),
                narrow(r.phase),
                narrow(r.derivative),
                narrow(r.phase_derivative),
                rotate(r.gain, 0, r.phase),
                rotate(r.radial, r.amplitude_phase_slope, r.phase)};
    }

    Complex transmit(Complex incident) const {
        return transmit(incident, std::hypot(incident.real(), incident.imag()));
    }

    Complex transmit(Complex incident, double total_radius) const {
        const double radius = std::hypot(incident.real(), incident.imag());
        if (!std::isfinite(incident.real()) || !std::isfinite(incident.imag()) ||
            !std::isfinite(radius) || !std::isfinite(total_radius) || total_radius < radius) {
            throw std::invalid_argument(
                "tabulated amplifier total radius must include incident wave");
        }
        const auto r = evaluate(total_radius);
        // Apply the same finite-response contract as response(), without narrowing
        // the phase before rotation or the amplitude before scaling the wave.
        response(total_radius);
        if (total_radius == 0) {
            return {};
        }
        return rotate(
            tabulated_amplifier_detail::scaled({incident.real(), r.amplitude}, {total_radius}),
            tabulated_amplifier_detail::scaled({incident.imag(), r.amplitude}, {total_radius}),
            r.phase);
    }
};
} // namespace rfmodel
