#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/linear_path.hpp"
#include "rfmodel/noise_figure.hpp"
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
    using namespace rfmodel;
    double loss_db = 1;
    double temperature_k = 290;
    if (argc > 3) {
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
    if (argc == 3) {
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
    const auto s = MatchedTransmissionModel("attenuator", loss_db).s_parameters(100e6);
    const auto path = analyze_linear_path({{"attenuator", s}}, 1e-19);
    const auto noise = passive_thermal_noise(s, temperature_k);
    // Noise factor retains its standard 290 K reference as physical temperature changes.
    const double noise_factor = std::pow(10., two_port_noise_figure_db(s, noise) / 10.);
    // Matched source thermal noise transmitted through S21 plus device excess noise.
    const double output_noise =
        1.380649e-23 * temperature_k * std::norm(s(1, 0)) + noise.watts_per_hz(1, 1).real();
    std::cout << std::setprecision(17) << "{\"gain\":" << *path.transducer_gain
              << ",\"noise_factor\":" << noise_factor
              << ",\"output_noise_w_per_hz\":" << output_noise
              << ",\"signal_output_w\":" << path.load_delivered_w << "}\n";
}
