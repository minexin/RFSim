#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/linear_path.hpp"
#include "rfmodel/linear_path_noise.hpp"
#include "rfmodel/noise_figure.hpp"
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
    using namespace rfmodel;
    double loss_db = 1;
    double temperature_k = 290;
    double source_available_w = 1e-19;
    if (argc > 4) {
        return 2;
    }
    if (argc >= 2) {
        try {
            std::size_t consumed = 0;
            loss_db = std::stod(argv[1], &consumed);
            if (consumed != std::string(argv[1]).size() || !std::isfinite(loss_db) || loss_db < 0 ||
                loss_db > 100) {
                return 2;
            }
        } catch (const std::exception &) {
            return 2;
        }
    }
    if (argc >= 3) {
        try {
            std::size_t consumed = 0;
            temperature_k = std::stod(argv[2], &consumed);
            if (consumed != std::string(argv[2]).size() || !std::isfinite(temperature_k) ||
                temperature_k <= 0 || temperature_k > 1000) {
                return 2;
            }
        } catch (const std::exception &) {
            return 2;
        }
    }
    if (argc == 4) {
        try {
            std::size_t consumed = 0;
            source_available_w = std::stod(argv[3], &consumed);
            if (consumed != std::string(argv[3]).size() || !std::isfinite(source_available_w) ||
                source_available_w < 1e-23 || source_available_w > 1) {
                return 2;
            }
        } catch (const std::exception &) {
            return 2;
        }
    }
    const auto s = MatchedTransmissionModel("attenuator", loss_db).s_parameters(100e6);
    const auto path = analyze_linear_path({{"attenuator", s}}, source_available_w);
    const auto noise = passive_thermal_noise(s, temperature_k);
    // Noise factor retains its standard 290 K reference as physical temperature changes.
    const auto noise_path = analyze_linear_path_noise({{"attenuator", s}}, {noise}, temperature_k);
    const double noise_factor = *noise_path.noise_factor;
    const double output_noise = noise_path.total_output_w_per_hz;
    std::cout << std::setprecision(17) << "{\"gain\":" << *path.transducer_gain
              << ",\"noise_factor\":" << noise_factor
              << ",\"output_noise_w_per_hz\":" << output_noise
              << ",\"signal_output_w\":" << path.load_delivered_w << "}\n";
}
