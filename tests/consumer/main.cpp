#include <rfmodel/linear_analysis.hpp>
#include <rfmodel/interpolation.hpp>
#include <rfmodel/measurements.hpp>
#include <rfmodel/noise.hpp>
#include <rfmodel/noise_figure.hpp>
#include <rfmodel/linear_solver.hpp>
#include <rfmodel/power_gain.hpp>
#include <rfmodel/linear_path_noise.hpp>
#include <rfmodel/loaded_noise.hpp>
#include <rfmodel/transmission_line.hpp>
#include <rfmodel/rlgc_transmission_line.hpp>
#include <rfmodel/multiport_devices.hpp>
#include <cmath>
#include <rfmodel/fundamental_compression.hpp>
#include <rfmodel/saturating_fundamental.hpp>
#include <rfmodel/single_tone_amplifier.hpp>

int main() {
    const rfmodel::SingleToneLimitedAmplifier single_tone(20., 20., 23., 20., 10.);
    const auto single_tone_output = single_tone.transmit({1e6, {{10, .001}}});
    if (single_tone_output.amplitudes.size() != 3 ||
        std::abs(std::norm(single_tone_output.amplitudes.at(20)) / 2.5e-10 - 1.) > 1e-12) {
        return 19;
    }
    const rfmodel::SaturatingFundamentalCompression saturation(20., 20., 23.);
    if (std::abs(std::norm(saturation.transmit_fundamental(.1)) / .19922937036162172 - 1.) >
        1e-12) {
        return 18;
    }
    const rfmodel::P1dBFundamentalCompression compressed(20., 10.);
    const std::vector<rfmodel::PowerWaveSpectrum> rf_inputs{{1e6, {{10, .001}, {20, .002}}},
                                                            {1e6, {{10, .003}}}};
    if (std::abs(rfmodel::incident_rf_power_watts(rf_inputs) / 14e-6 - 1.) > 1e-12 ||
        std::abs(compressed.transmit_fundamental_from_spectra(rf_inputs, 0, 10) -
                 compressed.transmit_fundamental(.001, 14e-6)) > 1e-12) {
        return 14;
    }
    const auto driven = compressed.transmit_fundamental(
        std::sqrt(compressed.input_p1db_watts() / 4.), compressed.input_p1db_watts());
    if (std::abs(std::norm(driven) / .0025 - 1.) > 1e-12) {
        return 12;
    }
    if (std::abs(
            std::norm(compressed.transmit_fundamental(std::sqrt(compressed.input_p1db_watts()))) /
                .01 -
            1.) > 1e-12) {
        return 11;
    }
    const auto loaded = rfmodel::loaded_noise({1, {0.}}, {{1, {1.}}}, {0.5}, {{1, {0.}}});
    if (std::abs(loaded.net_into_device_w_per_hz[0] + 0.75) > 1e-12) {
        return 10;
    }
    const rfmodel::RlgcTransmissionLineModel distributed("distributed", {25., 0., 0., 0.}, 2.);
    if (std::abs(distributed.s_parameters(0.)(1, 0) - rfmodel::Complex{2. / 3., 0.}) > 1e-12) {
        return 9;
    }
    const rfmodel::IsolatedPowerDividerModel phased("phased", {0.5, rfmodel::Complex{0., -0.5}});
    if (std::abs(phased.s_parameters(1e9)(2, 0) - rfmodel::Complex{0., -0.5}) > 1e-12) {
        return 8;
    }
    const rfmodel::EqualPowerDividerModel divider("divider", 4);
    const rfmodel::QuadratureCouplerModel coupler("coupler", 0.25);
    if (std::abs(divider.s_parameters(1e9)(1, 0) - rfmodel::Complex{0.5, 0.}) > 1e-12 ||
        std::abs(coupler.s_parameters(1e9)(2, 0) - rfmodel::Complex{0., 0.5}) > 1e-12) {
        return 7;
    }
    const rfmodel::TransmissionLineModel line("quarter wave", 100., 0.25e-9);
    if (std::abs(line.s_parameters(1e9)(0, 0) - rfmodel::Complex{0.6, 0.}) > 1e-12) {
        return 6;
    }
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
    const auto chain = rfmodel::analyze_linear_path_noise(
        {{"first", result.scattering[0]}, {"second", result.scattering[0]}}, {imported, imported});
    if (chain.contributions.size() != 2 || chain.contributions[0].name != "first" ||
        chain.contributions[1].name != "second") {
        return 5;
    }
    if (!chain.noise_factor || std::abs(*chain.noise_factor - 16.) > 1e-10 ||
        std::abs(chain.total_output_w_per_hz / (1.380649e-23 * 290.) - 1.) > 1e-10) {
        return 4;
    }
    if (std::abs(rfmodel::operating_power_gain(result.scattering[0]) - 0.25) >= 1e-12) {
        return 3;
    }
    return std::abs(nf - 10 * std::log10(4.)) < 1e-12 ? 0 : 2;
}
