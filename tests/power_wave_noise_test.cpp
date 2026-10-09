#include "rfmodel/power_wave_noise.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    const SMatrix active{2, {Complex{.2, -.1}, .03, Complex{2., 1.}, -.15}};
    const TwoPortNoiseParameters original{10. * std::log10(2.), {.2, .3}, 57.375};
    const auto noise = noise_from_parameters(active, original);
    for (const auto &references : std::vector<std::vector<Complex>>{
             {50., 50.}, {{25., 10.}, {100., -15.}}, {{75., -40.}, {30., 12.}}}) {
        const auto converted = renormalize_power_waves(active, {50., 50.}, references);
        const auto changed_noise = propagate_noise(converted.noise_transfer, noise);
        const auto parameters =
            extract_power_wave_noise_parameters(converted.scattering, changed_noise, references);
        near(parameters.minimum_noise_figure_db, original.minimum_noise_figure_db);
        near(parameters.noise_resistance_ohms, original.noise_resistance_ohms);
        const Complex optimum_z = 50. * (1. + original.optimum_source_reflection) /
                                  (1. - original.optimum_source_reflection);
        const Complex expected_gamma =
            (optimum_z - references[0]) / (optimum_z + std::conj(references[0]));
        near(parameters.optimum_source_reflection, expected_gamma);
        const auto recovered =
            noise_from_power_wave_parameters(converted.scattering, parameters, references);
        for (std::size_t i = 0; i < 4; ++i) {
            require(std::abs(recovered.watts_per_hz.values[i] -
                             changed_noise.watts_per_hz.values[i]) < 1e-32,
                    "noise covariance reconstruction");
        }
        for (Complex source : {Complex{50.}, Complex{25., 30.}, Complex{100., -40.}, optimum_z}) {
            const auto gamma = (source - 50.) / (source + 50.);
            const double actual =
                power_wave_noise_figure_db(converted.scattering, changed_noise, references, source);
            near(actual, two_port_noise_figure_db(active, noise, gamma));
            // Independent physical admittance form, F=Fmin+Rn/Gs*|Ys-Yopt|^2.
            const auto admittance = 1. / source, optimum_admittance = 1. / optimum_z;
            const double factor = std::pow(10., original.minimum_noise_figure_db / 10.) +
                                  original.noise_resistance_ohms / admittance.real() *
                                      std::norm(admittance - optimum_admittance);
            near(actual, 10. * std::log10(factor));
        }
        const NoiseCorrelation zero{SMatrix{2, {0., 0., 0., 0.}}};
        const auto noiseless =
            extract_power_wave_noise_parameters(converted.scattering, zero, references);
        near(noiseless.minimum_noise_figure_db, 0.);
        near(noiseless.noise_resistance_ohms, 0.);
        near(noiseless.optimum_source_reflection, 0.);
    }
    rejects<std::invalid_argument>([&] {
        power_wave_noise_figure_db(active, noise, {50., 50.}, Complex{0., 1.});
    });
    rejects<std::invalid_argument>([&] {
        extract_power_wave_noise_parameters(active, noise, {50.});
    });
    rejects<std::invalid_argument>([&] {
        extract_power_wave_noise_parameters(active, noise, {50., -1.});
    });
    rejects<std::invalid_argument>([&] {
        noise_from_power_wave_parameters(active, {1., 1., 10.}, {50., 50.});
    });
    rejects<std::invalid_argument>([&] {
        power_wave_noise_figure_db(active, noise, {50., 50.}, 50., 0.);
    });
    rejects<std::domain_error>([&] {
        extract_power_wave_noise_parameters(SMatrix{2, {0., 0., 0., 0.}}, noise, {50., 50.});
    });
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
