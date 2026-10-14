#include "rfmodel/nonlinear/phd_coefficient_slice.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <type_traits>

namespace {
using namespace rfmodel;
using Waves = std::vector<Complex>;

std::size_t checks = 0;

void require(bool condition, const std::string &message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void close(Complex actual, Complex expected, const std::string &message, double tolerance = 2e-12) {
    const double error = std::abs(actual - expected);
    require(std::isfinite(error) && error <= tolerance * (1. + std::abs(expected)), message);
}

template <class Exception, class Function>
void rejects(Function action, const std::string &message) {
    ++checks;
    try {
        action();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error("expected rejection: " + message);
}

SMatrix zero(std::size_t n) {
    return {n, Waves(n * n)};
}

// Integer multiplication, rather than the implementation's polar(m * theta),
// provides a separate oracle for positive AND negative phase exponents.
Complex power(Complex value, int exponent) {
    if (exponent < 0) {
        value = 1. / value;
        exponent = -exponent;
    }
    Complex result = 1.;
    for (int i = 0; i < exponent; ++i) {
        result *= value;
    }
    return result;
}

PhdCoefficientData fixture() {
    PhdCoefficientData data;
    data.fundamental_hz = 1.25e9;
    data.port_reference_ohms = {50., 75.};
    data.port_reference_planes = {"input:connector", "output:die"};
    // The last channel repeats harmonic 1 on another physical port. Channel
    // indices must never be mistaken for harmonic numbers or port numbers.
    data.channels = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 1}};
    data.pump_channel = 1;
    data.pump_magnitude_sqrt_w = .8;
    data.baseline_outgoing = {.7, {1.1, .2}, {-.3, .5}, {.4, -.6}, {-.2, .1}};
    data.direct = {5, {.2,          0., {.3, -.4},   {-.2, .1},   {.15, .05},
                       {.2, .1},    0., {.4, .2},    {-.15, .05}, {.1, -.2},
                       {-.1, .05},  0., {.25, -.2},  {-.1, .15},  {.2, .1},
                       {.04, -.08}, 0., {-.2, .3},   {.11, .07},  {-.08, .12},
                       {.1, .2},    0., {.06, -.09}, {.07, .03},  {.2, -.1}}};
    data.conjugate = {5, {.2,          0., {.3, .4},    {-.2, -.1},  {.15, -.05},
                          {.2, .1},    0., {-.1, .3},   {.2, -.1},   {.05, .1},
                          {-.1, .05},  0., {.12, .3},   {.04, -.06}, {-.1, .05},
                          {.04, -.08}, 0., {.09, -.02}, {-.03, .04}, {.05, .02},
                          {.1, .2},    0., {.02, .04},  {-.05, .02}, {-.06, .08}}};
    data.provenance = "independent complex F/S/T fixture";
    return data;
}

void hand_fixture_and_phase_covariance() {
    const auto data = fixture();
    const PhdCoefficientSlice slice(data);
    const Waves delta{.12, 0., {.03, -.04}, {-.02, .01}, {.05, .02}};
    // Fully expanded scalar complex arithmetic, fixed independently of the
    // implementation and of the phase-covariance checks below.
    const Waves expected{.753, {1.164, .2125}, {-.3298, .5201}, {.4137, -.5956}, {-.1687, .1493}};
    const auto phase_zero = slice.freeze_pump(0.);
    const auto unrotated = phase_zero.evaluate_perturbation(delta);
    for (std::size_t row = 0; row < data.channels.size(); ++row) {
        close(unrotated[row], expected[row], "hand-expanded complex F/S/T fixture");
    }

    const double pi = std::acos(-1.);
    for (const double theta : {0., .37, -.83, pi / 2., 2. * pi + .37, -4. * pi - .83}) {
        const Complex p{std::cos(theta), std::sin(theta)};
        const auto frozen = slice.freeze_pump(theta);
        const auto &jacobian = frozen.perturbation_jacobian();
        require(jacobian.size() == 5 && jacobian.pump_channel() == 1, "clamped Jacobian identity");
        Waves rotated_delta(5);
        for (std::size_t j = 0; j < 5; ++j) {
            rotated_delta[j] = delta[j] * power(p, data.channels[j].bin);
            close(frozen.nominal_incident()[j], j == 1 ? .8 * p : Complex{}, "nominal pump only");
        }
        const auto actual = frozen.evaluate_perturbation(rotated_delta);
        auto incident = frozen.nominal_incident();
        for (std::size_t j = 0; j < 5; ++j) {
            incident[j] += rotated_delta[j];
        }
        const auto total = frozen.evaluate_incident(incident);
        for (std::size_t row = 0; row < 5; ++row) {
            const int m = data.channels[row].bin;
            close(actual[row], expected[row] * power(p, m), "phase-covariant total response");
            close(total[row], actual[row], "total incidence and perturbation agree");
            close(frozen.nominal_outgoing()[row],
                  data.baseline_outgoing[row] * power(p, m),
                  "harmonic baseline phase");
            close(frozen.output_offset()[row],
                  frozen.nominal_outgoing()[row],
                  "affine output offset");
            for (std::size_t column = 0; column < 5; ++column) {
                const int n = data.channels[column].bin;
                close(jacobian.direct(row, column),
                      data.direct(row, column) * power(p, m - n),
                      "direct difference exponent, including negative exponents");
                close(jacobian.conjugate(row, column),
                      data.conjugate(row, column) * power(p, m + n),
                      "conjugate sum exponent");
            }
        }
        require(actual[0].imag() == 0. && frozen.nominal_outgoing()[0].imag() == 0.,
                "DC remains exactly real");
        require(frozen.coefficients().direct.values == data.direct.values &&
                    frozen.coefficients().conjugate.values == data.conjugate.values,
                "freezing must not overwrite native coefficients");
    }

    // The same channels in a different order must retain their physical meaning.
    const std::array<std::size_t, 5> order{{3, 4, 0, 2, 1}};
    auto reordered = data;
    reordered.pump_channel = 4;
    Waves reordered_delta(5);
    for (std::size_t i = 0; i < 5; ++i) {
        reordered.channels[i] = data.channels[order[i]];
        reordered.baseline_outgoing[i] = data.baseline_outgoing[order[i]];
        reordered_delta[i] = delta[order[i]];
        for (std::size_t j = 0; j < 5; ++j) {
            reordered.direct(i, j) = data.direct(order[i], order[j]);
            reordered.conjugate(i, j) = data.conjugate(order[i], order[j]);
        }
    }
    const auto reordered_result =
        PhdCoefficientSlice(reordered).freeze_pump(0.).evaluate_perturbation(reordered_delta);
    for (std::size_t i = 0; i < 5; ++i) {
        close(reordered_result[i], expected[order[i]], "channel-order independence");
    }
}

void finite_differences_and_affine_offset() {
    const auto data = fixture();
    const auto frozen = PhdCoefficientSlice(data).freeze_pump(.51);
    const auto &jacobian = frozen.perturbation_jacobian();
    constexpr double step = 1e-6;
    const Waves center{.07, 0., {.03, -.01}, {-.02, .04}, {.02, -.03}};
    for (std::size_t column = 0; column < 5; ++column) {
        if (column == data.pump_channel) {
            continue;
        }
        for (const Complex direction : {Complex{1., 0.}, Complex{0., 1.}}) {
            if (data.channels[column].bin == 0 && direction.imag() != 0.) {
                continue;
            }
            auto hi = center, lo = center;
            hi[column] += step * direction;
            lo[column] -= step * direction;
            const auto upper = frozen.evaluate_perturbation(hi),
                       lower = frozen.evaluate_perturbation(lo);
            Waves delta(5);
            delta[column] = direction;
            const auto applied = jacobian.apply(delta);
            for (std::size_t row = 0; row < 5; ++row) {
                const auto expected = jacobian.direct(row, column) * direction +
                                      jacobian.conjugate(row, column) * std::conj(direction);
                close((upper[row] - lower[row]) / (2. * step),
                      expected,
                      "every admissible quadrature derivative",
                      2e-10);
                close(applied[row], expected, "typed Jacobian action");
            }
        }
    }

    auto incident = frozen.nominal_incident();
    for (std::size_t j = 0; j < incident.size(); ++j) {
        incident[j] += center[j];
    }
    const auto evaluated = frozen.evaluate_incident(incident);
    for (std::size_t row = 0; row < incident.size(); ++row) {
        Complex affine = frozen.output_offset()[row];
        for (std::size_t column = 0; column < incident.size(); ++column) {
            affine += jacobian.direct(row, column) * incident[column] +
                      jacobian.conjugate(row, column) * std::conj(incident[column]);
        }
        close(evaluated[row], affine, "absolute-coordinate affine identity");
    }

    // Close one non-pump channel with a = source + gamma*b. Solve its real
    // two-by-two equations independently, then compare a converged iteration
    // that calls only the slice's checked evaluator. No network factory is used.
    constexpr std::size_t feedback = 4;
    const Complex source{.03, -.04}, gamma{.21, .07};
    const auto f = frozen.nominal_outgoing()[feedback];
    const auto a = jacobian.direct(feedback, feedback), b = jacobian.conjugate(feedback, feedback);
    const auto alpha = 1. - gamma * a, beta = -gamma * b, rhs = source + gamma * f;
    const double xx = (alpha + beta).real(), xy = -(alpha - beta).imag();
    const double yx = (alpha + beta).imag(), yy = (alpha - beta).real();
    const double determinant = xx * yy - xy * yx;
    const Complex expected_incident{(rhs.real() * yy - xy * rhs.imag()) / determinant,
                                    (xx * rhs.imag() - rhs.real() * yx) / determinant};
    Complex iterated{};
    for (int iteration = 0; iteration < 80; ++iteration) {
        Waves delta(5);
        delta[feedback] = iterated;
        iterated = source + gamma * frozen.evaluate_perturbation(delta)[feedback];
    }
    close(iterated, expected_incident, "independent scalar loaded feedback solve");
    Waves final_delta(5);
    final_delta[feedback] = iterated;
    const auto outgoing = frozen.evaluate_perturbation(final_delta)[feedback];
    close(outgoing,
          f + a * expected_incident + b * std::conj(expected_incident),
          "affine loaded outgoing");
    close(iterated - gamma * outgoing, source, "loaded feedback residual");
    require(std::abs(gamma * f) > .02, "fixture must exercise a consequential nonzero offset");
}

void direct_only_and_conjugate_only() {
    PhdCoefficientData data;
    data.fundamental_hz = 1e6;
    data.port_reference_ohms = {50.};
    data.port_reference_planes = {"native"};
    data.channels = {{0, 1}, {0, 2}, {0, 3}};
    data.pump_channel = 0;
    data.pump_magnitude_sqrt_w = 2.;
    data.baseline_outgoing = {{.4, .1}, {-.3, .2}, {.1, -.4}};
    data.direct = zero(3);
    data.conjugate = zero(3);
    const Complex gain{.6, -.2}, input{.03, -.04};
    constexpr double theta = .47;
    const Complex p{std::cos(theta), std::sin(theta)};
    for (const bool conjugate_only : {false, true}) {
        data.direct(1, 2) = conjugate_only ? Complex{} : gain;
        data.conjugate(1, 2) = conjugate_only ? gain : Complex{};
        const auto frozen = PhdCoefficientSlice(data).freeze_pump(theta);
        const Waves delta{0., 0., input};
        const auto variation = frozen.perturbation_jacobian().apply(delta);
        const auto expected =
            conjugate_only ? gain * power(p, 5) * std::conj(input) : gain * power(p, -1) * input;
        close(variation[1], expected, conjugate_only ? "pure conjugate law" : "direct linear law");
        close(frozen.evaluate_perturbation(delta)[1],
              data.baseline_outgoing[1] * power(p, 2) + expected,
              "linear/conjugate baseline remains affine");
        close(variation[0], 0., "no spurious pump-output tangent");
        close(variation[2], 0., "no spurious cross-channel tangent");
    }
}

void ordinary_lti_reduction() {
    // Ordinary two-port scattering matrices at four independent harmonics.
    // These are physical LTI coefficients before removing the pump coordinate
    // from the slice's admissible perturbation domain.
    const std::array<std::array<Complex, 4>, 4> scattering{
        {{{.1, .2, -.3, .4}},
         {{Complex{.12, .03}, Complex{.7, -.2}, Complex{.8, .15}, Complex{-.1, .04}}},
         {{Complex{.2, -.1}, Complex{.6, .04}, Complex{.5, -.2}, Complex{.08, .03}}},
         {{Complex{-.15, .05}, Complex{.4, -.3}, Complex{.3, .2}, Complex{.2, -.06}}}}};
    PhdCoefficientData data;
    data.fundamental_hz = 1e9;
    data.port_reference_ohms = {50., 75.};
    data.port_reference_planes = {"linear:input", "linear:output"};
    for (std::size_t port = 0; port < 2; ++port) {
        for (int harmonic = 0; harmonic < 4; ++harmonic) {
            data.channels.push_back({port, harmonic});
        }
    }
    data.pump_channel = 1;
    data.pump_magnitude_sqrt_w = .8;
    data.baseline_outgoing = Waves(8);
    data.direct = zero(8);
    data.conjugate = zero(8);
    for (std::size_t output_port = 0; output_port < 2; ++output_port) {
        data.baseline_outgoing[4 * output_port + 1] =
            scattering[1][2 * output_port] * data.pump_magnitude_sqrt_w;
        for (std::size_t input_port = 0; input_port < 2; ++input_port) {
            for (std::size_t h = 0; h < 4; ++h) {
                const auto row = 4 * output_port + h, column = 4 * input_port + h;
                if (column == data.pump_channel) {
                    continue;
                }
                const auto entry = scattering[h][2 * output_port + input_port];
                data.direct(row, column) = h == 0 ? .5 * entry : entry;
                data.conjugate(row, column) = h == 0 ? .5 * entry : Complex{};
            }
        }
    }
    const PhdCoefficientSlice slice(data);
    const Waves delta{.03, 0., {.02, -.01}, {-.03, .04}, -.02, {.05, .01}, {.04, .02}, {.01, -.02}};
    for (const double phase : {0., .41, -1.1, std::acos(-1.) / 2.}) {
        const auto frozen = slice.freeze_pump(phase);
        auto incident = frozen.nominal_incident();
        for (std::size_t i = 0; i < incident.size(); ++i) {
            incident[i] += delta[i];
        }
        const auto actual = frozen.evaluate_incident(incident);
        const auto perturbation_result = frozen.evaluate_perturbation(delta);
        for (std::size_t output_port = 0; output_port < 2; ++output_port) {
            for (std::size_t h = 0; h < 4; ++h) {
                // Direct ordinary-network equation; no slice coefficient or
                // Jacobian accessor participates in this expected response.
                const auto expected = scattering[h][2 * output_port] * incident[h] +
                                      scattering[h][2 * output_port + 1] * incident[4 + h];
                const auto row = 4 * output_port + h;
                close(actual[row], expected, "ordinary same-harmonic LTI network reduction");
                close(
                    perturbation_result[row], expected, "ordinary LTI perturbation-plus-baseline");
                for (std::size_t column = 0; column < 8; ++column) {
                    close(frozen.perturbation_jacobian().direct(row, column),
                          data.direct(row, column),
                          "ordinary LTI direct coefficients are phase independent");
                    close(frozen.perturbation_jacobian().conjugate(row, column),
                          data.conjugate(row, column),
                          "ordinary LTI has no positive-frequency conjugate response");
                }
            }
        }
    }
}

using LongComplex = std::complex<long double>;
using Spectrum = std::map<int, LongComplex>;
constexpr std::size_t harmonics = 6;

// Independent physical oracle: x(t) = a0 + sqrt(2)*Re sum(a_h exp(i*h*t)),
// y(t) = c*x(t)^3. Fourier coefficients are a_h/sqrt(2), with their negative
// frequency conjugates. An ordered triple convolution retains multiplicity;
// DC is projected without sqrt(2), positive outputs with sqrt(2).
Waves cubic_fourier(const Waves &incident, long double coefficient) {
    const long double root_two = std::sqrt(2.L);
    Spectrum x;
    x[0] = incident[0].real();
    for (std::size_t h = 1; h < harmonics; ++h) {
        const LongComplex value{incident[h].real(), incident[h].imag()};
        x[static_cast<int>(h)] = value / root_two;
        x[-static_cast<int>(h)] = std::conj(value) / root_two;
    }
    Spectrum y;
    for (const auto &a : x) {
        for (const auto &b : x) {
            for (const auto &c : x) {
                y[a.first + b.first + c.first] += coefficient * a.second * b.second * c.second;
            }
        }
    }
    Waves outgoing(2 * harmonics);
    for (std::size_t h = 0; h < harmonics; ++h) {
        const auto wave = y[static_cast<int>(h)] * (h == 0 ? 1.L : root_two);
        outgoing[harmonics + h] = {static_cast<double>(wave.real()),
                                   static_cast<double>(wave.imag())};
    }
    return outgoing;
}

PhdCoefficientData cubic_fixture(double coefficient, double radius) {
    PhdCoefficientData data;
    data.fundamental_hz = 1e8;
    data.port_reference_ohms = {50., 50.};
    data.port_reference_planes = {"cubic:input", "cubic:output"};
    for (std::size_t port = 0; port < 2; ++port) {
        for (std::size_t h = 0; h < harmonics; ++h) {
            data.channels.push_back({port, static_cast<int>(h)});
        }
    }
    data.pump_channel = 1;
    data.pump_magnitude_sqrt_w = radius;
    data.baseline_outgoing = Waves(2 * harmonics);
    // cos^3(t) = (3*cos(t) + cos(3*t))/4, in RMS coordinates.
    data.baseline_outgoing[harmonics + 1] = 1.5 * coefficient * radius * radius * radius;
    data.baseline_outgoing[harmonics + 3] = .5 * coefficient * radius * radius * radius;
    data.direct = zero(2 * harmonics);
    data.conjugate = zero(2 * harmonics);
    // d(c*x^3) = 3*c*x^2*dx. At a real pump, x^2 has coefficients
    // Q[0]=r^2 and Q[+/-2]=r^2/2. This analytic construction is separate
    // from the ordered cubic-convolution finite-difference oracle above.
    const auto square_coefficient = [radius](int h) {
        return h == 0 ? radius * radius : (std::abs(h) == 2 ? .5 * radius * radius : 0.);
    };
    for (std::size_t m = 0; m < harmonics; ++m) {
        for (std::size_t n = 0; n < harmonics; ++n) {
            if (n == data.pump_channel) {
                continue; // No F/r surrogate or pump-amplitude derivative exists.
            }
            const auto row = harmonics + m;
            if (n == 0) {
                const double derivative = 3. * coefficient *
                                          square_coefficient(static_cast<int>(m)) *
                                          (m == 0 ? 1. : std::sqrt(2.));
                data.direct(row, n) = data.conjugate(row, n) = .5 * derivative;
            } else {
                const double scale = 3. * coefficient / (m == 0 ? std::sqrt(2.) : 1.);
                data.direct(row, n) =
                    scale * square_coefficient(static_cast<int>(m) - static_cast<int>(n));
                data.conjugate(row, n) = scale * square_coefficient(static_cast<int>(m + n));
            }
        }
    }
    data.provenance = "analytic real cubic in native RMS wave coordinates";
    return data;
}

void independent_physical_cubic_oracle() {
    constexpr double coefficient = .7, radius = .8;
    const PhdCoefficientSlice slice(cubic_fixture(coefficient, radius));
    for (const double theta : {0., .39, -.91, std::acos(-1.) / 2.}) {
        const auto frozen = slice.freeze_pump(theta);
        const auto nominal = frozen.nominal_incident();
        const auto physical = cubic_fourier(nominal, coefficient);
        for (std::size_t row = 0; row < physical.size(); ++row) {
            close(frozen.nominal_outgoing()[row],
                  physical[row],
                  "cubic Fourier baseline and RMS scaling");
        }
        constexpr double step = 2e-6;
        for (std::size_t column = 0; column < nominal.size(); ++column) {
            if (column == 1) {
                continue;
            }
            for (const Complex direction : {Complex{1., 0.}, Complex{0., 1.}}) {
                if (column % harmonics == 0 && direction.imag() != 0.) {
                    continue;
                }
                auto hi = nominal, lo = nominal;
                hi[column] += step * direction;
                lo[column] -= step * direction;
                const auto upper = cubic_fourier(hi, coefficient),
                           lower = cubic_fourier(lo, coefficient);
                Waves direction_vector(nominal.size());
                direction_vector[column] = direction;
                const auto tangent = frozen.perturbation_jacobian().apply(direction_vector);
                for (std::size_t row = 0; row < nominal.size(); ++row) {
                    close(tangent[row],
                          (upper[row] - lower[row]) / (2. * step),
                          "physical cubic Fourier non-pump finite difference",
                          2e-10);
                }
                require(tangent[harmonics].imag() == 0., "cubic output DC tangent is exactly real");
            }
        }
    }
}

void invalid_metadata_and_dimensions() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    using Mutation = std::pair<std::string, std::function<void(PhdCoefficientData &)>>;
    const std::vector<Mutation> invalid{{"unknown convention",
                                         [](auto &d) {
                                             d.convention =
                                                 static_cast<PhdCoefficientConvention>(42);
                                         }},
                                        {"zero fundamental",
                                         [](auto &d) {
                                             d.fundamental_hz = 0.;
                                         }},
                                        {"negative fundamental",
                                         [](auto &d) {
                                             d.fundamental_hz = -1.;
                                         }},
                                        {"NaN fundamental",
                                         [=](auto &d) {
                                             d.fundamental_hz = nan;
                                         }},
                                        {"infinite fundamental",
                                         [=](auto &d) {
                                             d.fundamental_hz = inf;
                                         }},
                                        {"overflowing harmonic frequency",
                                         [](auto &d) {
                                             d.fundamental_hz = std::numeric_limits<double>::max();
                                         }},
                                        {"zero pump magnitude",
                                         [](auto &d) {
                                             d.pump_magnitude_sqrt_w = 0.;
                                         }},
                                        {"negative pump magnitude",
                                         [](auto &d) {
                                             d.pump_magnitude_sqrt_w = -.1;
                                         }},
                                        {"NaN pump magnitude",
                                         [=](auto &d) {
                                             d.pump_magnitude_sqrt_w = nan;
                                         }},
                                        {"infinite pump magnitude",
                                         [=](auto &d) {
                                             d.pump_magnitude_sqrt_w = inf;
                                         }},
                                        {"pump outside channels",
                                         [](auto &d) {
                                             d.pump_channel = d.channels.size();
                                         }},
                                        {"DC pump",
                                         [](auto &d) {
                                             d.pump_channel = 0;
                                         }},
                                        {"higher-harmonic pump",
                                         [](auto &d) {
                                             d.pump_channel = 2;
                                         }},
                                        {"missing channels",
                                         [](auto &d) {
                                             d.channels.clear();
                                         }},
                                        {"duplicate physical channel",
                                         [](auto &d) {
                                             d.channels[4] = d.channels[1];
                                         }},
                                        {"negative harmonic",
                                         [](auto &d) {
                                             d.channels[2].bin = -2;
                                         }},
                                        {"out-of-range physical port",
                                         [](auto &d) {
                                             d.channels[4].port = 2;
                                         }},
                                        {"missing physical port metadata",
                                         [](auto &d) {
                                             d.port_reference_ohms.clear();
                                         }},
                                        {"zero reference",
                                         [](auto &d) {
                                             d.port_reference_ohms[1] = 0.;
                                         }},
                                        {"negative reference",
                                         [](auto &d) {
                                             d.port_reference_ohms[1] = -50.;
                                         }},
                                        {"NaN reference",
                                         [=](auto &d) {
                                             d.port_reference_ohms[1] = nan;
                                         }},
                                        {"infinite reference",
                                         [=](auto &d) {
                                             d.port_reference_ohms[1] = inf;
                                         }},
                                        {"missing plane",
                                         [](auto &d) {
                                             d.port_reference_planes.pop_back();
                                         }},
                                        {"extra plane",
                                         [](auto &d) {
                                             d.port_reference_planes.push_back("extra");
                                         }},
                                        {"empty plane",
                                         [](auto &d) {
                                             d.port_reference_planes[0].clear();
                                         }},
                                        {"unrepresented physical port",
                                         [](auto &d) {
                                             d.port_reference_ohms.push_back(50.);
                                             d.port_reference_planes.push_back("unused");
                                         }},
                                        {"too many physical ports",
                                         [](auto &d) {
                                             d.port_reference_ohms.resize(1025, 50.);
                                             d.port_reference_planes.resize(1025, "plane");
                                         }},
                                        {"short baseline",
                                         [](auto &d) {
                                             d.baseline_outgoing.pop_back();
                                         }},
                                        {"long baseline",
                                         [](auto &d) {
                                             d.baseline_outgoing.push_back(0.);
                                         }},
                                        {"NaN baseline real part",
                                         [=](auto &d) {
                                             d.baseline_outgoing[2] = {nan, 0.};
                                         }},
                                        {"infinite baseline imaginary part",
                                         [=](auto &d) {
                                             d.baseline_outgoing[2] = {0., inf};
                                         }},
                                        {"complex DC baseline",
                                         [](auto &d) {
                                             d.baseline_outgoing[0] += Complex{0., 1e-30};
                                         }},
                                        {"wrong direct order",
                                         [](auto &d) {
                                             d.direct = zero(4);
                                         }},
                                        {"wrong conjugate order",
                                         [](auto &d) {
                                             d.conjugate = zero(4);
                                         }},
                                        {"short direct storage",
                                         [](auto &d) {
                                             d.direct.values.pop_back();
                                         }},
                                        {"long conjugate storage",
                                         [](auto &d) {
                                             d.conjugate.values.push_back(0.);
                                         }},
                                        {"NaN direct",
                                         [=](auto &d) {
                                             d.direct(2, 2) = {nan, 0.};
                                         }},
                                        {"infinite conjugate",
                                         [=](auto &d) {
                                             d.conjugate(3, 3) = {0., inf};
                                         }},
                                        {"nonzero direct pump column",
                                         [](auto &d) {
                                             d.direct(2, 1) = 1e-30;
                                         }},
                                        {"nonzero conjugate pump column",
                                         [](auto &d) {
                                             d.conjugate(2, 1) = Complex{0., 1e-30};
                                         }},
                                        {"noncanonical DC input split",
                                         [](auto &d) {
                                             d.direct(2, 0) += .01;
                                         }},
                                        {"non-real DC output tangent",
                                         [](auto &d) {
                                             d.conjugate(0, 2) += .01;
                                         }},
                                        {"complex DC-to-DC tangent", [](auto &d) {
                                             d.direct(0, 0) = d.conjugate(0, 0) = Complex{.1, .1};
                                         }}};
    for (const auto &test : invalid) {
        auto data = fixture();
        test.second(data);
        rejects<std::invalid_argument>(
            [&] {
                (void)PhdCoefficientSlice(data);
            },
            test.first);
    }

    // 512 is inclusive. Check it with valid, dense one-port metadata so the
    // upper-bound rejection cannot accidentally be supplied by another rule.
    auto maximum = fixture();
    maximum.port_reference_ohms = {50.};
    maximum.port_reference_planes = {"single physical port"};
    maximum.channels.clear();
    for (int h = 0; h < 512; ++h) {
        maximum.channels.push_back({0, h});
    }
    maximum.baseline_outgoing = Waves(512);
    maximum.direct = zero(512);
    maximum.conjugate = zero(512);
    const auto accepted = PhdCoefficientSlice(maximum).freeze_pump(.3);
    require(accepted.perturbation_jacobian().size() == 512 &&
                accepted.evaluate_perturbation(Waves(512)).size() == 512,
            "inclusive 512-channel limit");
    maximum.channels.push_back({0, 512});
    maximum.baseline_outgoing.push_back(0.);
    maximum.direct = zero(513);
    maximum.conjugate = zero(513);
    rejects<std::invalid_argument>(
        [&] {
            (void)PhdCoefficientSlice(maximum);
        },
        "513-channel limit");
}

void invalid_evaluation_and_overflow() {
    const PhdCoefficientSlice slice(fixture());
    const auto frozen = slice.freeze_pump(.4);
    const auto &jacobian = frozen.perturbation_jacobian();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double phase : {nan, inf, -inf}) {
        rejects<std::invalid_argument>(
            [&] {
                (void)slice.freeze_pump(phase);
            },
            "nonfinite phase");
    }
    const auto huge_phase = slice.freeze_pump(std::numeric_limits<double>::max());
    require(std::isfinite(huge_phase.pump_phase_radians()) &&
                std::abs(huge_phase.pump_phase_radians()) <= std::acos(-1.),
            "finite phase reduction");
    for (const auto count : {std::size_t{0}, std::size_t{4}, std::size_t{6}}) {
        rejects<std::invalid_argument>(
            [&] {
                (void)jacobian.apply(Waves(count));
            },
            "Jacobian delta dimensions");
        rejects<std::invalid_argument>(
            [&] {
                (void)frozen.evaluate_perturbation(Waves(count));
            },
            "slice delta dimensions");
        rejects<std::invalid_argument>(
            [&] {
                (void)frozen.evaluate_incident(Waves(count));
            },
            "total incidence dimensions");
    }
    for (const Complex value :
         {Complex{nan, 0.}, Complex{0., nan}, Complex{inf, 0.}, Complex{0., -inf}}) {
        for (std::size_t column = 0; column < 5; ++column) {
            Waves delta(5);
            delta[column] = value;
            rejects<std::invalid_argument>(
                [&] {
                    (void)jacobian.apply(delta);
                },
                "nonfinite perturbation");
            auto incident = frozen.nominal_incident();
            incident[column] = value;
            rejects<std::invalid_argument>(
                [&] {
                    (void)frozen.evaluate_incident(incident);
                },
                "nonfinite total incidence");
        }
    }
    for (const Complex value : {Complex{1e-30, 0.}, Complex{0., 1e-30}, Complex{-.1, .2}}) {
        Waves delta(5);
        delta[1] = value;
        rejects<std::invalid_argument>(
            [&] {
                (void)jacobian.apply(delta);
            },
            "pump-clamped Jacobian");
        rejects<std::invalid_argument>(
            [&] {
                (void)frozen.evaluate_perturbation(delta);
            },
            "pump-clamped slice");
    }
    auto incident = frozen.nominal_incident();
    incident[1] = {std::nextafter(incident[1].real(), inf), incident[1].imag()};
    rejects<std::invalid_argument>(
        [&] {
            (void)frozen.evaluate_incident(incident);
        },
        "even one-ULP pump change is unsupported");
    Waves imaginary_dc(5);
    imaginary_dc[0] = {0., 1e-30};
    rejects<std::invalid_argument>(
        [&] {
            (void)jacobian.apply(imaginary_dc);
        },
        "imaginary DC Jacobian direction");
    rejects<std::invalid_argument>(
        [&] {
            (void)frozen.evaluate_perturbation(imaginary_dc);
        },
        "imaginary DC perturbation");
    incident = frozen.nominal_incident();
    incident[0] = {0., 1e-30};
    rejects<std::invalid_argument>(
        [&] {
            (void)frozen.evaluate_incident(incident);
        },
        "imaginary DC total incidence");
    rejects<std::out_of_range>(
        [&] {
            (void)jacobian.direct(5, 0);
        },
        "direct row bounds");
    rejects<std::out_of_range>(
        [&] {
            (void)jacobian.direct(0, 5);
        },
        "direct column bounds");
    rejects<std::out_of_range>(
        [&] {
            (void)jacobian.conjugate(5, 0);
        },
        "conjugate row bounds");
    rejects<std::out_of_range>(
        [&] {
            (void)jacobian.conjugate(0, 5);
        },
        "conjugate column bounds");

    const double maximum = std::numeric_limits<double>::max();
    auto overflow = fixture();
    overflow.direct(2, 2) = maximum;
    overflow.conjugate(2, 2) = 0.;
    const auto large = PhdCoefficientSlice(overflow).freeze_pump(0.);
    Waves delta(5);
    delta[2] = 2.;
    rejects<std::overflow_error>(
        [&] {
            (void)large.perturbation_jacobian().apply(delta);
        },
        "nonfinite tangent result");
    overflow.direct(2, 2) = maximum / 2.;
    overflow.baseline_outgoing[2] = maximum;
    const auto large_offset = PhdCoefficientSlice(overflow).freeze_pump(0.);
    delta[2] = 1.5;
    rejects<std::overflow_error>(
        [&] {
            (void)large_offset.evaluate_perturbation(delta);
        },
        "nonfinite sum with baseline");
    overflow = fixture();
    overflow.direct(1, 2) = {maximum, maximum};
    rejects<std::overflow_error>(
        [&] {
            (void)PhdCoefficientSlice(overflow).freeze_pump(std::acos(-1.) / 4.);
        },
        "nonfinite rotated coefficient");
}

void ownership_and_restricted_types() {
    static_assert(std::is_same<decltype(std::declval<const PhdCoefficientSlice &>().coefficients()),
                               const PhdCoefficientData &>::value,
                  "slice metadata is read-only");
    static_assert(std::is_same<decltype(std::declval<const PhdFrozenSlice &>().nominal_incident()),
                               const Waves &>::value,
                  "frozen incident is read-only");
    static_assert(!std::is_convertible<PhdClampedJacobian, FrequencyConversionModel>::value,
                  "restricted tangent must not masquerade as a full Newton model");
    static_assert(!std::is_default_constructible<PhdClampedJacobian>::value,
                  "restricted Jacobian must originate from a validated slice");
    const Waves delta{.12, 0., {.03, -.04}, {-.02, .01}, {.05, .02}};
    auto original = fixture();
    const PhdCoefficientSlice slice(original);
    original.baseline_outgoing[2] = 1e6;
    original.direct(2, 2) = 1e6;
    original.port_reference_planes[0] = "mutated caller storage";
    require(slice.coefficients().port_reference_planes[0] == "input:connector",
            "owned metadata copy");
    close(slice.freeze_pump(0.).evaluate_perturbation(delta)[2],
          {-.3298, .5201},
          "caller mutation cannot change slice");

    const auto detached = [] {
        PhdCoefficientSlice temporary(fixture());
        return temporary.freeze_pump(.62);
    }();
    const auto expected =
        PhdCoefficientSlice(fixture()).freeze_pump(.62).evaluate_perturbation(delta);
    auto copied = detached;
    auto moved = std::move(copied);
    const auto actual = moved.evaluate_perturbation(delta);
    const auto copied_jacobian = detached.perturbation_jacobian();
    const auto orphaned_jacobian = [] {
        const auto temporary = PhdCoefficientSlice(fixture()).freeze_pump(.62);
        return temporary.perturbation_jacobian();
    }();
    for (std::size_t row = 0; row < actual.size(); ++row) {
        close(actual[row], expected[row], "frozen value survives source lifetime/copy/move");
        close(copied_jacobian.apply(delta)[row],
              actual[row] - moved.nominal_outgoing()[row],
              "owned typed tangent copy");
        close(orphaned_jacobian.apply(delta)[row],
              actual[row] - moved.nominal_outgoing()[row],
              "typed tangent survives frozen-owner lifetime");
    }
    const auto zero_phase_before = slice.freeze_pump(0.).evaluate_perturbation(delta);
    (void)slice.freeze_pump(-1.13);
    require(slice.freeze_pump(0.).evaluate_perturbation(delta) == zero_phase_before,
            "freezing another phase cannot contaminate the source slice");
}
} // namespace

int main() {
    try {
        hand_fixture_and_phase_covariance();
        finite_differences_and_affine_offset();
        direct_only_and_conjugate_only();
        ordinary_lti_reduction();
        independent_physical_cubic_oracle();
        invalid_metadata_and_dimensions();
        invalid_evaluation_and_overflow();
        ownership_and_restricted_types();
        std::cout << "Native pump-clamped PHD slice tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
