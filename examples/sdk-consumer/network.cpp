#include <rfmodel/linear/network.hpp>
#include <rfmodel/models/ideal_devices.hpp>
#include <rfmodel/models/transmission_line.hpp>
#include <cmath>
#include <stdexcept>

// Host adapter: 6 dB attenuator followed by a matched, lossless 1 ns line.
// Frequencies are Hz, available source power is W, all references are 50 ohms.
// The returned vectors own their data; no model/network lifetime escapes here.
rfmodel::NetworkWaves simulate_matched_link(double frequency_hz, double available_power_w) {
    if (!std::isfinite(available_power_w) || available_power_w < 0.) {
        throw std::invalid_argument("source power must be finite and nonnegative");
    }
    const rfmodel::MatchedTransmissionModel attenuator("pad", 6.);
    const rfmodel::TransmissionLineModel cable("line", 50., 1e-9);
    rfmodel::LinearNetwork network;
    const auto pad = network.add(attenuator.s_parameters(frequency_hz));
    const auto line = network.add(cable.s_parameters(frequency_hz));
    network.connect(pad + 1, line);
    // a = source + Gamma*b. Matched source/load have Gamma = 0.
    // RMS power-wave amplitudes are sqrt(W), not volts or peak amplitudes.
    network.terminate(pad, {}, {std::sqrt(available_power_w), 0.});
    network.terminate(line + 1);
    // Global order: pad input/output (0,1), line input/output (2,3).
    return network.solve();
}
