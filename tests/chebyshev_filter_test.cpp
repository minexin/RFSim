#include "rfmodel/chebyshev_filter.hpp"
#include "rfmodel/rlc_model.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace rfmodel;
constexpr double pi = 3.14159265358979323846;
constexpr std::array<FilterResponse, 4> responses{FilterResponse::Lowpass,
                                                  FilterResponse::Highpass,
                                                  FilterResponse::Bandpass,
                                                  FilterResponse::Bandstop};

ChebyshevFilterParameters
parameters(FilterResponse response, std::size_t order, double ripple, double attenuation) {
    ChebyshevFilterParameters p;
    p.response = response;
    p.order = order;
    p.ripple_db = ripple;
    p.passband_attenuation_db = attenuation;
    p.upper_passband_hz =
        response == FilterResponse::Bandpass || response == FilterResponse::Bandstop ? 4e9 : 0.;
    return p;
}

// Textbook analog poles, with direct arithmetic at ordinary frequencies. Do not
// use the model's logarithmic mapping or reflection-polynomial implementation.
Complex analog_poles(double mapped, std::size_t order, double ripple, double attenuation) {
    const double epsilon = std::sqrt(std::expm1(ripple * std::log(10.) / 10.));
    const double edge_epsilon = std::sqrt(std::expm1(attenuation * std::log(10.) / 10.));
    mapped *= std::cosh(std::acosh(edge_epsilon / epsilon) / double(order));
    const double mu = std::asinh(1. / epsilon) / double(order);
    Complex expected = order % 2 ? 1. : std::pow(10., -ripple / 20.);
    for (std::size_t k = 0; k < order; ++k) {
        const double theta = (2. * double(k) + 1.) * pi / (2. * double(order));
        const Complex pole{-std::sinh(mu) * std::sin(theta), std::cosh(mu) * std::cos(theta)};
        expected *= -pole / (Complex{0., mapped} - pole);
    }
    return expected;
}

void lossless_reciprocal(const SMatrix &s) {
    require(s(1, 0) == s(0, 1), "Chebyshev exact reciprocity");
    for (std::size_t port = 0; port < 2; ++port) {
        require(std::abs(std::norm(s(port, 0)) + std::norm(s(port, 1)) - 1.) < 1e-12,
                "Chebyshev unitary row power");
    }
    require(std::abs(s(0, 0) * std::conj(s(1, 0)) + s(0, 1) * std::conj(s(1, 1))) < 1e-14,
            "Chebyshev unitary cross term");
    for (const auto value : passive_thermal_noise(s, 290.).watts_per_hz.values) {
        require(std::abs(value) < 1e-32, "four-response lossless Chebyshev thermal noise");
    }
}

void test_four_mappings_and_topology() {
    for (const auto response : responses) {
        for (const std::size_t order : {2u, 3u, 8u, 16u, 64u}) {
            for (const double ripple : {.01, .1, 1., 10.}) {
                // Separate attenuation from ripple as well as exercising the
                // usual equal-edge case. Scaling the analog poles is the oracle.
                for (const double attenuation : {ripple, 2. * ripple}) {
                    auto p = parameters(response, order, ripple, attenuation);
                    const ChebyshevFilterModel open("open", p);
                    p.input_stopband_open = false;
                    const ChebyshevFilterModel shorted("short", p);
                    for (const double f : {.1, .7, 1., 1.5, 3., 4., 10.}) {
                        double mapped = f;
                        switch (response) {
                        case FilterResponse::Lowpass:
                            break;
                        case FilterResponse::Highpass:
                            mapped = -1. / f;
                            break;
                        case FilterResponse::Bandpass:
                            mapped = (f * f - 4.) / (3. * f);
                            break;
                        case FilterResponse::Bandstop:
                            mapped = -3. * f / (f * f - 4.);
                            break;
                        }
                        const auto expected = analog_poles(mapped, order, ripple, attenuation);
                        const auto a = open.s_parameters(f * 1e9);
                        const auto b = shorted.s_parameters(f * 1e9);
                        for (const auto &s : {a, b}) {
                            require(std::abs(s(1, 0) - expected) < 3e-12,
                                    "four-response independent Chebyshev pole mapping");
                            lossless_reciprocal(s);
                            require(s(1, 1) == (order % 2 ? s(0, 0) : -s(0, 0)),
                                    "Chebyshev even/odd reflection parity");
                        }
                        require(a(1, 0) == b(1, 0), "dual topology preserves transmission");
                        require(a(0, 0) == -b(0, 0) && a(1, 1) == -b(1, 1),
                                "dual topology reverses both reflections");
                    }
                }
            }
        }
    }
}

void test_independent_edges_and_dc_parity() {
    for (const auto response : responses) {
        const bool band =
            response == FilterResponse::Bandpass || response == FilterResponse::Bandstop;
        for (const std::size_t order : {2u, 3u, 8u, 9u}) {
            for (const bool open : {false, true}) {
                for (const double attenuation : {.1, 1., 10.}) {
                    auto p = parameters(response, order, .1, attenuation);
                    p.input_stopband_open = open;
                    const ChebyshevFilterModel model("edges", p);
                    for (const double f :
                         (band ? std::vector<double>{1e9, 4e9} : std::vector<double>{1e9})) {
                        require(std::abs(std::norm(model.s_parameters(f)(1, 0)) -
                                         std::pow(10., -attenuation / 10.)) < 5e-12,
                                "Chebyshev independent passband edge attenuation");
                    }
                }
                auto p = parameters(response, order, 1., 1.);
                p.input_stopband_open = open;
                const ChebyshevFilterModel model("parity", p);
                const double pass_power = order % 2 ? 1. : std::pow(10., -.1);
                const auto dc = model.s_parameters(0.);
                const bool dc_pass =
                    response == FilterResponse::Lowpass || response == FilterResponse::Bandstop;
                require(std::abs(std::norm(dc(1, 0)) - (dc_pass ? pass_power : 0.)) < 5e-13,
                        "Chebyshev DC limit and even-order mismatch");
                lossless_reciprocal(dc);
                if (band) {
                    const auto center = model.s_parameters(2e9);
                    require(std::abs(std::norm(center(1, 0)) -
                                     (response == FilterResponse::Bandpass ? pass_power : 0.)) <
                                5e-13,
                            "Chebyshev geometric band center and parity");
                    lossless_reciprocal(center);
                }
                // Include the mapping's zero/infinite limits in topology checks.
                p.input_stopband_open = !open;
                const ChebyshevFilterModel dual("dual", p);
                for (const double f : {0., 2e9, 1e308}) {
                    const auto a = model.s_parameters(f), b = dual.s_parameters(f);
                    require(a(1, 0) == b(1, 0) && a(0, 0) == -b(0, 0) && a(1, 1) == -b(1, 1),
                            "Chebyshev limiting dual topology");
                }
            }
        }
    }
}

void test_extreme_band_edges_and_dc() {
    for (const double low : {1e-200, 1e200}) {
        const double high = std::nextafter(low, std::numeric_limits<double>::infinity());
        for (const auto response : {FilterResponse::Bandpass, FilterResponse::Bandstop}) {
            for (const std::size_t order : {2u, 3u, 64u}) {
                auto p = parameters(response, order, .1, .1);
                p.lower_passband_hz = low;
                p.upper_passband_hz = high;
                for (const bool open : {false, true}) {
                    p.input_stopband_open = open;
                    const ChebyshevFilterModel model("adjacent band edges", p);
                    for (const double f : {low, high}) {
                        const double bandpass_map = f == low ? -1. : 1.;
                        const double mapped =
                            response == FilterResponse::Bandpass ? bandpass_map : -bandpass_map;
                        const auto s = model.s_parameters(f);
                        require(std::abs(std::norm(s(1, 0)) - std::pow(10., -.01)) < 5e-13,
                                "adjacent extreme Chebyshev band-edge attenuation");
                        require(std::abs(s(1, 0) - analog_poles(mapped, order, .1, .1)) < 3e-12,
                                "adjacent extreme band-edge signed pole response");
                        lossless_reciprocal(s);
                    }
                }
            }
        }
    }
    const double smallest = std::numeric_limits<double>::denorm_min();
    require(smallest > 0., "Chebyshev regression requires representable subnormals");
    for (const double ripple : {smallest, 1e-12, 100., 6000.}) {
        for (const std::size_t order : {2u, 3u, 64u}) {
            auto p = parameters(FilterResponse::Lowpass, order, ripple, ripple);
            const ChebyshevFilterModel model("extreme ripple", p);
            for (const double f : {0., smallest, 1e-300, 1e9, 1e308}) {
                const auto s = model.s_parameters(f);
                for (const auto value : s.values) {
                    require(std::isfinite(value.real()) && std::isfinite(value.imag()),
                            "extreme Chebyshev response stays finite");
                }
                lossless_reciprocal(s);
            }
            // Compare amplitudes, not powers: at 6000 dB the DC transmission
            // of even orders is representable (1e-300), but its power is not.
            const double dc_amplitude = order % 2 ? 1. : std::pow(10., -ripple / 20.);
            require(std::abs(std::abs(model.s_parameters(0.)(1, 0)) / dc_amplitude - 1.) < 2e-12,
                    "extreme Chebyshev representable DC transmission");
        }
    }
    auto p = parameters(FilterResponse::Lowpass, 3, 6000., 6000.);
    // T3(x) = 4*x^3 - 3*x, epsilon ~= 1e300 and x = 1e-300.
    require(std::abs(std::norm(ChebyshevFilterModel("near DC", p).s_parameters(1e-291)(1, 0)) -
                     .1) < 5e-13,
            "odd Chebyshev polynomial retains its linear term near DC");
    p.lower_passband_hz = 1e300;
    const ChebyshevFilterModel subnormal("underflowed ratio", p);
    // x = 1e-608 cannot be represented, but epsilon*T3(x) ~= -3e-308 can.
    require(std::abs(std::abs(subnormal.s_parameters(1e-308)(0, 0)) / 3e-308 - 1.) < 2e-12,
            "underflowed frequency ratio retains representable reflection");
    const double tiny_reflection = std::abs(subnormal.s_parameters(smallest)(0, 0));
    // Here the reflection itself is subnormal. One denormal ULP bounds final
    // rounding without accepting its disappearance to zero.
    require(tiny_reflection > 0. && std::abs(tiny_reflection - 3. * smallest) <= smallest,
            "Chebyshev retains subnormal reflected amplitude");
}
} // namespace

int run() {
    test_four_mappings_and_topology();
    test_independent_edges_and_dc_parity();
    test_extreme_band_edges_and_dc();
    using namespace rfmodel;
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t order : {2u, 3u, 4u, 9u, 32u, 64u}) {
        for (double ripple : {.01, .1, 1., 10.}) {
            ChebyshevFilterParameters p;
            p.order = order;
            p.ripple_db = p.passband_attenuation_db = ripple;
            ChebyshevFilterModel model("filter", p);
            const double epsilon = std::sqrt(std::expm1(ripple * std::log(10.) / 10.));
            const double mu = std::asinh(1. / epsilon) / double(order);
            for (double ratio : {0., .1, .7, 1., 1.1, 2., 10.}) {
                const auto s = model.s_parameters(ratio * 1e9);
                double previous = 1., current = ratio;
                for (std::size_t k = 2; k <= order; ++k) {
                    const double next = 2. * ratio * current - previous;
                    previous = current;
                    current = next;
                }
                const double expected = 1. / (1. + epsilon * epsilon * current * current);
                require(std::abs(std::norm(s(1, 0)) - expected) < 3e-12,
                        "Chebyshev recurrence magnitude");
                Complex h = order % 2 ? 1. : std::pow(10., -ripple / 20.);
                for (std::size_t k = 0; k < order; ++k) {
                    const double theta = (2. * double(k) + 1.) * pi / (2. * double(order));
                    const Complex pole{-std::sinh(mu) * std::sin(theta),
                                       std::cosh(mu) * std::cos(theta)};
                    h *= -pole / (Complex{0., ratio} - pole);
                }
                require(std::abs(h - s(1, 0)) < 3e-12, "Chebyshev analog pole response");
                for (auto v : passive_thermal_noise(s, 290.).watts_per_hz.values) {
                    require(std::abs(v) < 1e-32, "lossless Chebyshev noise");
                }
            }
        }
    }
    // Even-order realization with unequal terminal references. Its reference
    // change is equivalent to an ideal transformer at the output port.
    for (double ripple : {.01, .1, 1., 10.}) {
        const double mu = std::asinh(1. / std::sqrt(std::pow(10., ripple / 10.) - 1.)) / 2.;
        const double gamma = std::sinh(mu);
        const double g1 = std::sqrt(2.) / gamma;
        const double g2 = 2. / ((gamma * gamma + 1.) * g1);
        const double load = 1. / std::pow(std::tanh(mu), 2.);
        ChebyshevFilterParameters p;
        p.order = 2;
        p.ripple_db = p.passband_attenuation_db = ripple;
        ChebyshevFilterModel model("even", p);
        for (double x : {0., .2, .7, 1., 2.}) {
            const Complex a = 1. - x * x * g1 * g2;
            const Complex b{0., x * g1}, c{0., x * g2};
            const Complex denominator = a * load + b + c * load + 1.;
            const auto actual = model.s_parameters(x * 1e9);
            near(actual(0, 0), (a * load + b - c * load - 1.) / denominator);
            near(actual(1, 1), (-a * load + b - c * load + 1.) / denominator);
            near(actual(1, 0), 2. * std::sqrt(load) / denominator);
        }
    }
    // Independent third-order LC ladder synthesis at 1 dB ripple.
    const double gamma = std::sinh(std::asinh(1. / std::sqrt(std::pow(10., .1) - 1.)) / 3.);
    const double g1 = 1. / gamma;
    const double g2 = 2. / ((gamma * gamma + .75) * g1);
    const double omega = 2. * pi * 1e9;
    ChebyshevFilterParameters p;
    p.ripple_db = p.passband_attenuation_db = 1.;
    ChebyshevFilterModel filter("filter", p);
    for (double f : {0., 1e8, 7e8, 1e9, 2e9}) {
        LinearNetwork net;
        net.add(
            IdealRLCModel(
                "l1", IdealElement::Inductor, LumpedConnection::SeriesImpedance, g1 * 50. / omega)
                .s_parameters(f));
        net.add(
            IdealRLCModel(
                "c", IdealElement::Capacitor, LumpedConnection::ShuntAdmittance, g2 / (50. * omega))
                .s_parameters(f));
        net.add(
            IdealRLCModel(
                "l2", IdealElement::Inductor, LumpedConnection::SeriesImpedance, g1 * 50. / omega)
                .s_parameters(f));
        net.connect(1, 2);
        net.connect(3, 4);
        const auto actual = filter.s_parameters(f);
        const auto expected = net.external_s({0, 5});
        for (std::size_t i = 0; i < 4; ++i) {
            near(actual.values[i], expected.values[i]);
        }
    }
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
