#include "rfmodel/chebyshev_filter.hpp"
#include "rfmodel/rlc_model.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
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
