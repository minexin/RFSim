#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/linear_path.hpp"
#include "rfmodel/noise_figure.hpp"
#include <iomanip>
#include <iostream>

int main() {
    using namespace rfmodel;
    const auto s = MatchedTransmissionModel("attenuator", 1).s_parameters(100e6);
    const auto path = analyze_linear_path({{"attenuator", s}}, 1e-19);
    const auto noise = passive_thermal_noise(s, 290);
    const double noise_factor = std::pow(10., two_port_noise_figure_db(s, noise) / 10.);
    // Matched source thermal noise transmitted through S21 plus device excess noise.
    const double output_noise =
        1.380649e-23 * 290 * std::norm(s(1, 0)) + noise.watts_per_hz(1, 1).real();
    std::cout << std::setprecision(17) << "{\"gain\":" << *path.transducer_gain
              << ",\"noise_factor\":" << noise_factor
              << ",\"output_noise_w_per_hz\":" << output_noise
              << ",\"signal_output_w\":" << path.load_delivered_w << "}\n";
}
