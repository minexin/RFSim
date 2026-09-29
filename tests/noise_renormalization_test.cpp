#include "rfmodel/noise_renormalization.hpp"
#include "rfmodel/noise_figure.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290.;
    const SMatrix pad{2, {Complex{.1, .05}, .4, .4, -.1}};
    const auto thermal = passive_thermal_noise(pad, 290.);
    const auto changed_s = renormalize_s(pad, 75., 50.);
    const auto changed_c = renormalize_noise(pad, thermal, 75., 50.);
    const auto expected = passive_thermal_noise(changed_s, 290.);
    const auto restored = renormalize_noise(changed_s, changed_c, 50., 75.);
    for (std::size_t i = 0; i < 4; ++i) {
        near(changed_c.watts_per_hz.values[i] / kt, expected.watts_per_hz.values[i] / kt);
        near(restored.watts_per_hz.values[i] / kt, thermal.watts_per_hz.values[i] / kt);
    }
    const SMatrix active{2, {Complex{.1, .1}, .02, Complex{3., .2}, -.1}};
    const auto noise = noise_from_parameters(active, {3., Complex{.2, .1}, 100.}, 75.);
    const auto renormalized_s = renormalize_s(active, 75., 50.);
    const auto renormalized_c = renormalize_noise(active, noise, 75., 50.);
    // Hold the physical source impedance fixed while changing its wave reference.
    for (Complex source : {Complex{}, Complex{.3, -.1}}) {
        const auto transformed_source = (source + .2) / (1. + .2 * source);
        near(two_port_noise_figure_db(active, noise, source),
             two_port_noise_figure_db(renormalized_s, renormalized_c, transformed_source));
    }
    near(renormalize_noise(SMatrix{1, {0.}}, NoiseCorrelation{SMatrix{1, {kt}}}, 75., 50.)
                 .watts_per_hz(0, 0) /
             kt,
         .96);
    rejects<std::invalid_argument>([&] {
        renormalize_noise(active, noise, 0., 50.);
    });
    rejects<std::invalid_argument>([&] {
        renormalize_noise(active, NoiseCorrelation{SMatrix{1, {-1.}}}, 75., 50.);
    });
    rejects<std::domain_error>([&] {
        renormalize_noise(SMatrix{1, {-5.}}, NoiseCorrelation{SMatrix{1, {kt}}}, 75., 50.);
    });
}
