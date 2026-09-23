#include "rfmodel/noise_figure.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290.;
    const SMatrix s{2, {Complex{0.1, 0.2}, Complex{0.05, -0.1}, Complex{2., -1.}, -0.2}};
    const Complex gamma{0.2, 0.3};
    const TwoPortNoiseParameters parameters{
        10 * std::log10(2.), gamma, 75. * 3. * std::norm(1. + gamma) / 4.};
    const auto noise = noise_from_parameters(s, parameters, 75.);
    const auto recovered = extract_noise_parameters(s, noise, 75.);
    near(recovered.minimum_noise_figure_db, parameters.minimum_noise_figure_db);
    near(recovered.optimum_source_reflection, gamma);
    near(recovered.noise_resistance_ohms, parameters.noise_resistance_ohms);
    const auto warmer = noise_from_parameters(s, parameters, 75., 580.);
    for (std::size_t i = 0; i < 4; ++i) {
        near(warmer.watts_per_hz.values[i] / kt, 2. * noise.watts_per_hz.values[i] / kt);
    }
    const SMatrix pad{2, {0., 0.5, 0.5, 0.}};
    const auto thermal = passive_thermal_noise(pad, 290.);
    const auto imported = noise_from_parameters(pad, extract_noise_parameters(pad, thermal));
    for (std::size_t i = 0; i < 4; ++i) {
        near(imported.watts_per_hz.values[i] / kt, thermal.watts_per_hz.values[i] / kt);
    }
    // Imported device data enters the same assembled network solver.
    LinearNetwork network;
    network.add(pad);
    network.add(pad);
    network.connect(1, 2);
    const auto output = network.external_noise({0, 3}, independent_noise({imported, imported}));
    near(two_port_noise_figure_db(network.external_s({0, 3}), output), 10 * std::log10(16.));
    for (const Complex source : {Complex{}, Complex{0.3, -0.4}, Complex{-0.7, 0.1}}) {
        const double expected = 2. + 3. * std::norm(source - gamma) / (1. - std::norm(source));
        near(two_port_noise_figure_db(s, noise, source), 10 * std::log10(expected));
    }
    // Nonnegative NFmin/Rn can still describe an indefinite covariance.
    rejects<std::invalid_argument>([&] {
        noise_from_parameters(s, {3., {}, 0.});
    });
    rejects<std::invalid_argument>([&] {
        noise_from_parameters(s, {0., 1., 1.});
    });
    rejects<std::invalid_argument>([&] {
        noise_from_parameters(s, parameters, 0.);
    });
    rejects<std::overflow_error>([&] {
        noise_from_parameters(s, {1e308, {}, 1.});
    });
    const auto zero = noise_from_parameters(s, {});
    for (auto value : zero.watts_per_hz.values) {
        near(value, 0.);
    }
}
