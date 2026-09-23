#include <rfmodel/linear_analysis.hpp>
#include <rfmodel/interpolation.hpp>
#include <rfmodel/measurements.hpp>
#include <rfmodel/noise.hpp>
#include <rfmodel/noise_figure.hpp>
#include <rfmodel/linear_solver.hpp>
#include <rfmodel/power_gain.hpp>
#include <cmath>

int main() {
    auto result = rfmodel::analyze_linear(
        rfmodel::FrequencyGrid{{1e9}},
        {0, 1},
        [](double) {
            rfmodel::LinearNetwork network;
            network.add(rfmodel::SMatrix{2, {0., 0.5, 0.5, 0.}});
            return network;
        },
        [](double, const rfmodel::LinearNetwork &) {
            return rfmodel::passive_thermal_noise(rfmodel::SMatrix{2, {0., 0.5, 0.5, 0.}}, 290.);
        });
    if (std::abs(result.scattering[0](1, 0) - rfmodel::Complex{0.5, 0}) >= 1e-12) {
        return 1;
    }
    const auto parameters =
        rfmodel::extract_noise_parameters(result.scattering[0], result.noise_correlation[0]);
    const auto imported = rfmodel::noise_from_parameters(result.scattering[0], parameters);
    const auto nf = rfmodel::two_port_noise_figure_db(result.scattering[0], imported);
    if (std::abs(rfmodel::operating_power_gain(result.scattering[0]) - 0.25) >= 1e-12) {
        return 3;
    }
    return std::abs(nf - 10 * std::log10(4.)) < 1e-12 ? 0 : 2;
}
