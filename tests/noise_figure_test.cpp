#include "rfmodel/noise_figure.hpp"
#include "rfmodel/noise.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290.;
    const SMatrix pad{2, {0., 0.5, 0.5, 0.}};
    const auto noise = passive_thermal_noise(pad, 290.);
    near(two_port_noise_figure_db(pad, noise), 10 * std::log10(4.));
    near(two_port_noise_figure_db(pad, passive_thermal_noise(pad, 580.)), 10 * std::log10(7.));
    near(two_port_noise_figure_db(pad, noise, 0.5),
         10 * std::log10(1. + 0.75 * (4. + 0.25) / 0.75));
    const SMatrix unity{2, {0., 0., 1., 0.}};
    const NoiseCorrelation correlated{SMatrix{2, {kt, Complex{0., -kt}, Complex{0., kt}, kt}}};
    near(two_port_noise_figure_db(unity, correlated, Complex{0., 0.5}), 10 * std::log10(4.));
    near(two_port_noise_figure_db(unity, correlated, Complex{0., -0.5}), 10 * std::log10(4. / 3));
    // Matched cascade against independent scalar Friis implementation.
    const SMatrix amplifier{2, {0., 0., std::sqrt(10.), 0.}};
    const NoiseCorrelation amp_noise{SMatrix{2, {0., 0., 0., 10. * kt}}}; // F=2
    LinearNetwork net;
    net.add(pad);
    net.add(amplifier);
    net.connect(1, 2);
    const auto combined = independent_noise({noise, amp_noise});
    near(two_port_noise_figure_db(net.external_s({0, 3}), net.external_noise({0, 3}, combined)),
         cascade_noise_figure_db(
             {{-10 * std::log10(4.), 10 * std::log10(4.)}, {10., 10 * std::log10(2.)}}));
    rejects<std::invalid_argument>([&] {
        two_port_noise_figure_db(pad, noise, 1.);
    });
    rejects<std::invalid_argument>([&] {
        two_port_noise_figure_db(pad, noise, 0., 0.);
    });
    rejects<std::domain_error>([&] {
        two_port_noise_figure_db(SMatrix{2, {0., 0., 0., 0.}}, noise);
    });
    rejects<std::domain_error>([&] {
        two_port_noise_figure_db(SMatrix{2, {2., 0., 1., 0.}}, noise, 0.5);
    });
}
