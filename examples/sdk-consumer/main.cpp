#include <rfmodel/analysis/measurements.hpp>
#include <rfmodel/linear/network.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

// Implemented in a separate translation unit, as in a host application adapter.
rfmodel::NetworkWaves simulate_matched_link(double frequency_hz, double available_power_w);

namespace {
void require(bool condition, const char *message) {
    // Keep checks active in Release builds (no assert/NDEBUG dependency).
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    try {
        const double source_w = rfmodel::dbm_to_watts(0.); // 0 dBm = 1 mW
        bool rejected = false;
        try {
            (void)simulate_matched_link(-1., source_w);
        } catch (const std::invalid_argument &error) {
            rejected = true;
            std::cout << "Expected invalid-frequency error: " << error.what() << '\n';
        }
        require(rejected, "negative frequency was not rejected");

        // Continue with valid requests after the host handles an invalid request.
        for (double frequency_hz : {100e6, 250e6, 500e6}) {
            const auto waves = simulate_matched_link(frequency_hz, source_w);
            require(waves.incident.size() == 4 && waves.outgoing.size() == 4,
                    "unexpected global port count");
            // Network port 3 points into the line: outgoing power is delivered
            // to the external matched load; absorbed power has the opposite sign.
            const auto output = rfmodel::port_power(waves.incident[3], waves.outgoing[3]);
            const double load_w = output.outgoing_w - output.incident_w;
            const double gain_db = rfmodel::power_gain_db(load_w, source_w);
            const auto expected_wave =
                std::polar(std::sqrt(source_w) * std::pow(10., -6. / 20.),
                           -2. * 3.14159265358979323846 * frequency_hz * 1e-9);
            require(std::abs(waves.outgoing[3] - expected_wave) < 1e-12,
                    "output amplitude/phase differs from analytic matched cascade");
            require(std::abs(waves.outgoing[0]) < 1e-12 && std::abs(waves.incident[3]) < 1e-12,
                    "source/load are not matched");
            require(std::abs(load_w - source_w * std::pow(10., -6. / 10.)) < 1e-15 &&
                        std::abs(gain_db + 6.) < 1e-10,
                    "delivered power or gain differs from analytic attenuation");
            require(std::isfinite(waves.relative_residual) && waves.relative_residual < 1e-10,
                    "network residual is too large");
            std::cout << frequency_hz << " Hz: load=" << load_w << " W ("
                      << rfmodel::watts_to_dbm(load_w) << " dBm), gain=" << gain_db
                      << " dB, outgoing=" << waves.outgoing[3] << " sqrt(W)\n";
        }
        return 0;
    } catch (const std::exception &error) {
        // Translate unexpected SDK/host errors to the application's failure path.
        std::cerr << "SDK integration failed: " << error.what() << '\n';
        return 1;
    }
}
