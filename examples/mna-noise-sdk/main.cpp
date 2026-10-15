#include "mna_noise.hpp"
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
    try {
        mna_noise_example::Circuit circuit;
        if (argc == 2 && std::string(argv[1]) == "--variant") {
            circuit.resistance_ohm = 11.;
            circuit.inductance_h *= 1.5;
            circuit.temperature_k = 307.;
        } else if (argc != 1) {
            throw std::invalid_argument("usage: mna_noise [--variant]");
        }
        mna_noise_example::verify_failure_and_ownership_cases();
        const auto result = mna_noise_example::run(circuit);
        std::cout << std::setprecision(12) << "source-free series RL: R=" << circuit.resistance_ohm
                  << " ohm L=" << circuit.inductance_h << " H T=" << circuit.temperature_k << " K\n"
                  << "frequency_Hz,S21_real,S21_imag,C11_W_per_Hz,C12_real_W_per_Hz,load_voltage_"
                     "real_V,load_voltage_imag_V,resistor_power_W\n";
        for (const auto &sample : result.samples) {
            std::cout << sample.frequency_hz << ',' << sample.scattering(1, 0).real() << ','
                      << sample.scattering(1, 0).imag() << ','
                      << sample.intrinsic_noise.watts_per_hz(0, 0).real() << ','
                      << sample.intrinsic_noise.watts_per_hz(0, 1).real() << ','
                      << sample.load_voltage_v.real() << ',' << sample.load_voltage_v.imag() << ','
                      << sample.resistor_power_w << '\n';
        }
        std::cout << "PASS S_absolute_error=" << result.worst_s_absolute_error
                  << " Johnson_error_over_kT=" << result.worst_johnson_error_over_kt
                  << " equilibrium_flow_over_kT=" << result.worst_equilibrium_flow_over_kt
                  << " conversion_channels=" << result.snapshot.model.channels().size()
                  << "; full C/P, unequal-reference cascade, joint S/C renormalization, ownership "
                     "and negative checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
