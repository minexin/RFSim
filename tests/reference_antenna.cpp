#include "rfmodel/amplifier_model.hpp"
#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/linear_path_noise.hpp"
#include <iomanip>
#include <iostream>

// Small-signal mapping of the official Antenna Noise Temperature example.
// This is not a complete RFAMP model: matched forward noise only, no compression.
int main(int argc, char **argv) {
    using namespace rfmodel;
    constexpr double frequency = 5e9;
    constexpr double boltzmann = 1.380649e-23;
    constexpr double reference_temperature = 290.;
    double source_noise_density = boltzmann * 50.;
    if (argc > 2) {
        return 2;
    }
    if (argc == 2) {
        try {
            std::size_t consumed = 0;
            source_noise_density = std::stod(argv[1], &consumed);
            if (consumed != std::string(argv[1]).size() || !std::isfinite(source_noise_density) ||
                source_noise_density <= 0 || source_noise_density > 1e-12) {
                return 2;
            }
        } catch (const std::exception &) {
            return 2;
        }
    }
    const double source_temperature = source_noise_density / boltzmann;
    constexpr double source_power = 1e-8;
    const double losses[] = {1. + 77.7 / 290., 1. + 453.6 / 290.};
    const double gains[] = {std::pow(10., 2.5), 1000.};
    const double noise_factors[] = {1. + 150. / 290., 1. + 700. / 290.};
    std::vector<LinearPathStage> stages;
    std::vector<NoiseCorrelation> noises;
    for (std::size_t index = 0; index < 2; ++index) {
        const auto suffix = std::to_string(index + 1);
        const auto pad = MatchedTransmissionModel("Attn" + suffix, 10. * std::log10(losses[index]))
                             .s_parameters(frequency);
        stages.push_back({"Attn" + suffix, pad});
        noises.push_back(passive_thermal_noise(pad, reference_temperature));
        LinearAmplifierParameters parameters;
        parameters.gain_db = index == 0 ? 25. : 30.;
        parameters.reverse_isolation_db = 50.;
        const auto amp = LinearAmplifierModel("RFAmp" + suffix, parameters).s_parameters(frequency);
        stages.push_back({"RFAmp" + suffix, amp});
        const double added =
            boltzmann * reference_temperature * gains[index] * (noise_factors[index] - 1.);
        noises.push_back({{2, {0., 0., 0., added}}});
    }
    std::cout << std::setprecision(17)
              << "{\"scope\":\"matched small-signal four-stage approximation\",\"nodes\":["
              << "{\"name\":\"Source\",\"gain\":1,\"noise_factor\":1,\"signal_output_w\":"
              << source_power << ",\"output_noise_w_per_hz\":" << source_noise_density << "}";
    double expected_gain = 1.;
    double expected_factor = 1.;
    for (std::size_t end = 1; end <= stages.size(); ++end) {
        const std::vector<LinearPathStage> prefix(stages.begin(), stages.begin() + end);
        const std::vector<NoiseCorrelation> covariance(noises.begin(), noises.begin() + end);
        const auto signal = analyze_linear_path(prefix, source_power);
        const auto noise = analyze_linear_path_noise(prefix, covariance, source_temperature);
        const std::size_t device = (end - 1) / 2;
        const bool is_pad = end % 2 == 1;
        const double factor = is_pad ? losses[device] : noise_factors[device];
        expected_factor += (factor - 1.) / expected_gain;
        expected_gain *= is_pad ? 1. / losses[device] : gains[device];
        const double expected_noise =
            boltzmann * expected_gain *
            (source_temperature + reference_temperature * (expected_factor - 1.));
        if (std::abs(*noise.noise_factor / expected_factor - 1.) > 1e-12 ||
            std::abs(noise.total_output_w_per_hz / expected_noise - 1.) > 1e-12 ||
            std::abs(signal.load_delivered_w / (source_power * expected_gain) - 1.) > 1e-12) {
            std::cerr << "Independent Friis reference failed at stage " << end << '\n';
            return 1;
        }
        std::cout << ",{\"name\":\"" << stages[end - 1].name
                  << "\",\"gain\":" << *signal.transducer_gain
                  << ",\"noise_factor\":" << *noise.noise_factor
                  << ",\"signal_output_w\":" << signal.load_delivered_w
                  << ",\"output_noise_w_per_hz\":" << noise.total_output_w_per_hz << "}";
    }
    std::cout << "]}\n";
}
