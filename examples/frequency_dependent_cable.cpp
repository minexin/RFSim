// C++17, header-only. Link with RFModel::rfmodel; no C ABI, Python, or JSON.
#include <rfmodel/frequency_dependent_cable.hpp>
#include <rfmodel/noise_matrix.hpp>
#include <array>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
    try {
        const bool with_noise = argc == 2 && std::string(argv[1]) == "--noise";
        if (argc > 2 || (argc == 2 && !with_noise)) {
            throw std::invalid_argument("usage: frequency_dependent_cable_example [--noise]");
        }
        rfmodel::FrequencyDependentCableParameters parameters;
        parameters.length_m = 2.5;
        parameters.characteristic_ohms = 75.;
        parameters.relative_permittivity = 2.25;
        parameters.reference_frequency_hz = 2e9;
        // Independent contributions in dB/m at reference_frequency_hz.
        parameters.sqrt_loss_db_per_m = .6;
        parameters.linear_loss_db_per_m = .15;
        parameters.reference_ohms = 60.;
        const rfmodel::FrequencyDependentCableModel cable("engineering cable", parameters);
        constexpr double temperature_k = 290.;
        constexpr std::array<double, 6> frequencies{0., 1e6, 1e8, 1e9, 2.5e9, 6e9};

        std::cout << std::setprecision(17) << "frequency_Hz,propagation_loss_dB";
        for (const char *entry : {"S11", "S12", "S21", "S22"}) {
            std::cout << ',' << entry << "_re," << entry << "_im";
        }
        if (with_noise) {
            std::cout << ",temperature_K";
            for (const char *entry : {"C11", "C12", "C21", "C22"}) {
                std::cout << ',' << entry << "_re_W_per_Hz," << entry << "_im_W_per_Hz";
            }
        }
        std::cout << '\n';
        for (double frequency_hz : frequencies) {
            const auto s = cable.s_parameters(frequency_hz);
            std::cout << frequency_hz << ','
                      << rfmodel::cable_propagation_loss_db(frequency_hz, parameters);
            // Row-major: outgoing port first, incident port second.
            for (const auto value : s.values) {
                std::cout << ',' << value.real() << ',' << value.imag();
            }
            if (with_noise) {
                // Existing passive noise semantics and tolerances apply.
                const auto noise = rfmodel::passive_thermal_noise(s, temperature_k);
                std::cout << ',' << temperature_k;
                for (const auto value : noise.watts_per_hz.values) {
                    std::cout << ',' << value.real() << ',' << value.imag();
                }
            }
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "frequency-dependent cable example: " << error.what() << '\n';
        return 1;
    }
}
