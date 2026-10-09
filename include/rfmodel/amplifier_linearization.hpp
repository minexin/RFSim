#pragma once
#include "conversion_operating_point.hpp"
#include "saturating_fundamental.hpp"

namespace rfmodel {
// Matched unilateral fundamental compression. All RF bins on the selected
// drive ports share one gain; no new harmonic/intermodulation bins are generated.
// Input/output port bin sets must match. DC is present only as an inactive zero.
inline ConversionLinearization
linearize_saturating_amplifier(double spacing_hz,
                               const std::vector<ConversionChannel> &channels,
                               const std::vector<Complex> &incident,
                               const SaturatingFundamentalCompression &amplifier,
                               std::size_t input_port = 0,
                               std::size_t output_port = 1,
                               const std::vector<std::size_t> &drive_ports = {},
                               double reference_ohms = 50.) {
    const auto n = channels.size();
    if (!n || n > 512 || incident.size() != n || input_port >= 1024 || output_port >= 1024 ||
        input_port == output_port) {
        throw std::invalid_argument("invalid amplifier linearization parameters");
    }
    auto direct = conversion_detail::zero(n), conjugate = direct;
    // Validate grid, references, duplicate channel identities, and positive ranges.
    FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference_ohms);
    std::set<std::size_t> driven;
    const auto selected = drive_ports.empty() ? std::vector<std::size_t>{input_port} : drive_ports;
    for (auto port : selected) {
        if ((port != input_port && port != output_port) || !driven.insert(port).second) {
            throw std::invalid_argument("invalid or repeated amplifier drive port");
        }
    }
    if (!driven.count(input_port)) {
        throw std::invalid_argument("amplifier drive must include the input port");
    }
    std::map<int, std::size_t> inputs, outputs;
    std::vector<std::size_t> drive_channels;
    double power = 0.;
    bool rf = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto channel = channels[i];
        if ((channel.port != input_port && channel.port != output_port) ||
            !conversion_detail::finite(incident[i]) ||
            (channel.bin == 0 && incident[i] != Complex{})) {
            throw std::invalid_argument("invalid amplifier wave/port or nonzero DC");
        }
        auto &indices = channel.port == input_port ? inputs : outputs;
        indices.emplace(channel.bin, i);
        if (channel.bin > 0 && driven.count(channel.port)) {
            rf = true;
            power += std::norm(incident[i]);
            if (!std::isfinite(power)) {
                throw std::overflow_error("amplifier total drive overflow");
            }
            drive_channels.push_back(i);
        }
    }
    if (!rf || inputs.size() != outputs.size()) {
        throw std::invalid_argument("amplifier requires matching RF port bins");
    }
    const auto response = amplifier.gain_response(power);
    const double radius = std::sqrt(power);
    std::vector<Complex> outgoing(n);
    for (const auto &entry : inputs) {
        const auto found = outputs.find(entry.first);
        if (found == outputs.end()) {
            throw std::invalid_argument("amplifier input/output frequency grids differ");
        }
        if (entry.first == 0) {
            continue;
        }
        const auto input = entry.second, output = found->second;
        outgoing[output] = amplifier.transmit_fundamental(incident[input], power);
        direct(output, input) = response.amplitude_gain;
        if (power > 0.) {
            const auto row = (response.radial_gain_difference * .5) * (incident[input] / radius);
            for (auto column : drive_channels) {
                const auto direction = incident[column] / radius;
                direct(output, column) += row * std::conj(direction);
                conjugate(output, column) += row * direction;
            }
        }
    }
    return {FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference_ohms),
            outgoing};
}
} // namespace rfmodel
