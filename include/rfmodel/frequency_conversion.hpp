#pragma once
#include "network_parameters.hpp"
#include "noise_matrix.hpp"
#include <set>
#include <limits>

namespace rfmodel {
struct ConversionChannel {
    std::size_t port{};
    int bin{};
};

// C=E[z*z^H], P=E[z*z^T], both in W/Hz. P is necessary for improper noise.
struct ConversionNoise {
    SMatrix covariance;
    SMatrix complementary;
};

struct ConversionResult {
    std::vector<Complex> incident, outgoing;
    ConversionNoise outgoing_noise;
    double relative_residual{};
};

namespace conversion_detail {
inline SMatrix zero(std::size_t n) {
    return {n, std::vector<Complex>(n * n)};
}

inline bool finite(Complex x) {
    return std::isfinite(x.real()) && std::isfinite(x.imag());
}

inline SMatrix multiply(const SMatrix &a, const SMatrix &b) {
    auto result = zero(a.ports);
    for (std::size_t i = 0; i < a.ports; ++i) {
        for (std::size_t k = 0; k < a.ports; ++k) {
            if (a(i, k) == Complex{}) {
                continue;
            }
            for (std::size_t j = 0; j < a.ports; ++j) {
                result(i, j) += a(i, k) * b(k, j);
            }
        }
    }
    noise_detail::finite_matrix(result);
    return result;
}

inline ConversionNoise complex_noise(const SMatrix &q) {
    const auto n = q.ports / 2;
    ConversionNoise result{zero(n), zero(n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const double xx = q(2 * i, 2 * j).real(), yy = q(2 * i + 1, 2 * j + 1).real();
            const double xy = q(2 * i, 2 * j + 1).real(), yx = q(2 * i + 1, 2 * j).real();
            result.covariance(i, j) = {xx + yy, yx - xy};
            result.complementary(i, j) = {xx - yy, yx + xy};
        }
    }
    noise_detail::finite_matrix(result.covariance);
    noise_detail::finite_matrix(result.complementary);
    return result;
}
} // namespace conversion_detail

// Fixed-pump, finite-channel model b=A*a+B*conj(a)+c. Channels retain physical
// port and frequency identity. All waves use a common positive real reference.
class FrequencyConversionModel {
    double spacing_, reference_;
    std::vector<ConversionChannel> channels_;
    SMatrix direct_, conjugate_, quadrature_;

    SMatrix quadrature_noise(const ConversionNoise &noise) const {
        const auto n = channels_.size();
        noise_detail::finite_matrix(noise.covariance);
        noise_detail::finite_matrix(noise.complementary);
        if (noise.covariance.ports != n || noise.complementary.ports != n) {
            throw std::invalid_argument("conversion noise dimensions differ");
        }
        auto q = conversion_detail::zero(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                const auto c = noise.covariance(i, j) * .5;
                const auto p = noise.complementary(i, j) * .5;
                q(2 * i, 2 * j) = c.real() + p.real();
                q(2 * i + 1, 2 * j + 1) = c.real() - p.real();
                q(2 * i, 2 * j + 1) = p.imag() - c.imag();
                q(2 * i + 1, 2 * j) = p.imag() + c.imag();
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (channels_[i].bin != 0) {
                continue;
            }
            for (std::size_t j = 0; j < 2 * n; ++j) {
                if (q(2 * i + 1, j) != Complex{} || q(j, 2 * i + 1) != Complex{}) {
                    throw std::invalid_argument("DC noise must be real");
                }
            }
        }
        return noise_detail::psd_matrix(q);
    }

public:
    FrequencyConversionModel(double spacing_hz,
                             std::vector<ConversionChannel> channels,
                             SMatrix direct,
                             SMatrix conjugate,
                             double reference_ohms = 50.)
        : spacing_(spacing_hz), reference_(reference_ohms), channels_(std::move(channels)),
          direct_(std::move(direct)), conjugate_(std::move(conjugate)) {
        const auto n = channels_.size();
        if (!n || n > 512 || !std::isfinite(spacing_) || spacing_ <= 0. ||
            !std::isfinite(reference_) || reference_ <= 0.) {
            throw std::invalid_argument("invalid conversion channels or reference");
        }
        noise_detail::finite_matrix(direct_);
        noise_detail::finite_matrix(conjugate_);
        if (direct_.ports != n || conjugate_.ports != n) {
            throw std::invalid_argument("conversion matrix dimensions differ");
        }
        std::set<std::pair<std::size_t, int>> seen;
        for (const auto &channel : channels_) {
            if (channel.port >= 1024 || channel.bin < 0 || !std::isfinite(channel.bin * spacing_) ||
                !seen.emplace(channel.port, channel.bin).second) {
                throw std::invalid_argument("invalid or duplicate conversion channel");
            }
        }
        quadrature_ = conversion_detail::zero(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                const auto sum = direct_(i, j) + conjugate_(i, j);
                const auto difference = direct_(i, j) - conjugate_(i, j);
                quadrature_(2 * i, 2 * j) = sum.real();
                quadrature_(2 * i + 1, 2 * j) = sum.imag();
                if (channels_[j].bin != 0) {
                    quadrature_(2 * i, 2 * j + 1) = -difference.imag();
                    quadrature_(2 * i + 1, 2 * j + 1) = difference.real();
                }
            }
        }
        noise_detail::finite_matrix(quadrature_);
        for (std::size_t i = 0; i < n; ++i) {
            if (channels_[i].bin != 0) {
                continue;
            }
            for (std::size_t j = 0; j < 2 * n; ++j) {
                if (quadrature_(2 * i + 1, j) != Complex{}) {
                    throw std::invalid_argument("conversion must produce real DC waves");
                }
            }
        }
    }

    const SMatrix &direct() const {
        return direct_;
    }

    const SMatrix &conjugate() const {
        return conjugate_;
    }

    const std::vector<ConversionChannel> &channels() const {
        return channels_;
    }

    ConversionNoise zero_noise() const {
        return {conversion_detail::zero(channels_.size()),
                conversion_detail::zero(channels_.size())};
    }

    // a=Gamma*b+source. Source and intrinsic noises are independent; correlations
    // within either set, including across frequencies, are retained in C and P.
    ConversionResult analyze(const std::vector<Complex> &source,
                             const std::vector<Complex> &reflection,
                             const ConversionNoise &source_noise,
                             const ConversionNoise &intrinsic_noise) const {
        const auto n = channels_.size(), m = 2 * n;
        if (source.size() != n || reflection.size() != n) {
            throw std::invalid_argument("conversion boundary dimensions differ");
        }
        auto source_q = quadrature_noise(source_noise);
        auto intrinsic_q = quadrature_noise(intrinsic_noise);
        auto equation = conversion_detail::zero(m), identity = equation;
        for (std::size_t j = 0; j < n; ++j) {
            if (!conversion_detail::finite(source[j]) ||
                !conversion_detail::finite(reflection[j]) ||
                (channels_[j].bin == 0 && (source[j].imag() != 0. || reflection[j].imag() != 0.))) {
                throw std::invalid_argument("invalid conversion boundary or complex DC");
            }
            for (std::size_t i = 0; i < m; ++i) {
                equation(i, 2 * j) = -quadrature_(i, 2 * j) * reflection[j].real() -
                                     quadrature_(i, 2 * j + 1) * reflection[j].imag();
                equation(i, 2 * j + 1) = quadrature_(i, 2 * j) * reflection[j].imag() -
                                         quadrature_(i, 2 * j + 1) * reflection[j].real();
            }
        }
        for (std::size_t i = 0; i < m; ++i) {
            equation(i, i) += 1.;
            identity(i, i) = 1.;
        }
        noise_detail::finite_matrix(equation);
        const auto inverse = parameter_detail::solve(equation, identity, 1.);
        const auto transfer = conversion_detail::multiply(inverse, quadrature_);
        ConversionResult result;
        result.incident.resize(n);
        result.outgoing.resize(n);
        std::vector<double> outgoing(m);
        for (std::size_t i = 0; i < m; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                outgoing[i] += transfer(i, 2 * j).real() * source[j].real() +
                               transfer(i, 2 * j + 1).real() * source[j].imag();
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            result.outgoing[i] = {outgoing[2 * i], outgoing[2 * i + 1]};
            result.incident[i] = reflection[i] * result.outgoing[i] + source[i];
            if (!conversion_detail::finite(result.outgoing[i]) ||
                !conversion_detail::finite(result.incident[i])) {
                throw std::overflow_error("conversion waves overflow");
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            Complex residual = -result.outgoing[i];
            double scale = std::abs(result.outgoing[i]);
            for (std::size_t j = 0; j < n; ++j) {
                const auto first = direct_(i, j) * result.incident[j];
                const auto second = conjugate_(i, j) * std::conj(result.incident[j]);
                residual += first + second;
                scale += std::abs(first) + std::abs(second);
            }
            if (!std::isfinite(scale) || !conversion_detail::finite(residual)) {
                throw std::overflow_error("conversion residual overflow");
            }
            if (scale > 0.) {
                result.relative_residual =
                    std::max(result.relative_residual, std::abs(residual) / scale);
            }
        }
        if (result.relative_residual > 512 * n * std::numeric_limits<double>::epsilon()) {
            throw std::domain_error("conversion residual exceeds tolerance");
        }
        auto total = propagate_noise(transfer, {source_q}).watts_per_hz;
        const auto intrinsic = propagate_noise(inverse, {intrinsic_q}).watts_per_hz;
        for (std::size_t i = 0; i < total.values.size(); ++i) {
            total.values[i] += intrinsic.values[i];
        }
        result.outgoing_noise = conversion_detail::complex_noise(total);
        return result;
    }
};

// Existing real-mixer convention: y=2*g*x*cos(LO*t+phase). Every generated IF
// channel must be declared; no implicit image rejection or frequency truncation.
inline FrequencyConversionModel
ideal_mixer_conversion(double spacing_hz,
                       const std::vector<ConversionChannel> &channels,
                       int lo_bin,
                       double gain_db,
                       double phase_radians = 0.,
                       std::size_t rf_port = 0,
                       std::size_t if_port = 1,
                       double reference_ohms = 50.) {
    if (channels.empty() || channels.size() > 512 || lo_bin <= 0 || rf_port == if_port ||
        !std::isfinite(gain_db) || !std::isfinite(phase_radians) ||
        !std::isfinite(lo_bin * spacing_hz)) {
        throw std::invalid_argument("invalid conversion mixer parameters");
    }
    const double gain = std::pow(10., gain_db / 20.);
    if (!std::isfinite(gain) || gain == 0.) {
        throw std::overflow_error("conversion mixer gain range");
    }
    const auto pump = std::polar(gain, phase_radians);
    auto a = conversion_detail::zero(channels.size()), b = a;
    for (const auto &channel : channels) {
        if (channel.port != rf_port && channel.port != if_port) {
            throw std::invalid_argument("mixer channels must belong to RF or IF port");
        }
    }
    bool has_rf = false;
    auto add = [&](long long bin, std::size_t input, Complex direct, Complex conjugate) {
        if (bin < 0 || bin > std::numeric_limits<int>::max()) {
            throw std::overflow_error("conversion mixer bin range");
        }
        for (std::size_t row = 0; row < channels.size(); ++row) {
            if (channels[row].port == if_port && channels[row].bin == bin) {
                a(row, input) += direct;
                b(row, input) += conjugate;
                return;
            }
        }
        throw std::invalid_argument("missing generated mixer output channel");
    };
    for (std::size_t column = 0; column < channels.size(); ++column) {
        if (channels[column].port != rf_port) {
            continue;
        }
        has_rf = true;
        const auto bin = channels[column].bin;
        if (bin == 0) {
            add(lo_bin, column, std::sqrt(2.) * pump, {});
            continue;
        }
        add(static_cast<long long>(bin) + lo_bin, column, pump, {});
        const auto difference = static_cast<long long>(bin) - lo_bin;
        if (difference > 0) {
            add(difference, column, std::conj(pump), {});
        } else if (difference < 0) {
            add(-difference, column, {}, pump);
        } else {
            add(0, column, std::conj(pump) / std::sqrt(2.), pump / std::sqrt(2.));
        }
    }
    if (!has_rf) {
        throw std::invalid_argument("conversion mixer has no RF channel");
    }
    return FrequencyConversionModel(
        spacing_hz, channels, std::move(a), std::move(b), reference_ohms);
}
} // namespace rfmodel
