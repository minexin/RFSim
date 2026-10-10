#pragma once
#include "coherent_highorder_amplifier.hpp"
#include "polynomial_linearization.hpp"

namespace rfmodel {
// Resolved physical RF waves: every input bin drives common compression;
// generating_bins alone generate new products. Other input bins propagate with
// the same direct gain. This does not track multiple origins at one frequency.
// DC is blocked and its incident wave must be zero. Output-side drive is ignored.
inline ConversionLinearization
linearize_highorder_amplifier(double spacing_hz,
                              const std::vector<ConversionChannel> &channels,
                              const std::vector<Complex> &incident,
                              const CoherentHighOrderAmplifier &amplifier,
                              const std::vector<int> &generating_bins,
                              std::size_t input_port = 0,
                              std::size_t output_port = 1) {
    const auto n = channels.size();
    if (!n || n > 512 || incident.size() != n || input_port >= 1024 || output_port >= 1024 ||
        input_port == output_port) {
        throw std::invalid_argument("invalid high-order linearization parameters");
    }
    auto direct = conversion_detail::zero(n), conjugate = direct;
    const double reference = amplifier.reference_ohms();
    FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference);
    std::map<int, std::size_t> inputs, outputs;
    double power = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const auto channel = channels[i];
        if ((channel.port != input_port && channel.port != output_port) ||
            !conversion_detail::finite(incident[i]) ||
            (channel.bin == 0 && incident[i] != Complex{})) {
            throw std::invalid_argument("invalid high-order port/wave or nonzero DC");
        }
        if (channel.bin == 0) {
            continue;
        }
        auto &indices = channel.port == input_port ? inputs : outputs;
        indices.emplace(channel.bin, i);
        if (channel.port == input_port) {
            power += std::norm(incident[i]);
            if (!std::isfinite(power)) {
                throw std::overflow_error("high-order total drive overflow");
            }
        }
    }
    if (inputs.empty() || outputs.empty()) {
        throw std::invalid_argument("high-order amplifier requires RF input/output channels");
    }
    for (const auto &input : inputs) {
        if (!outputs.count(input.first)) {
            throw std::invalid_argument("missing high-order direct output channel");
        }
    }
    std::set<int> generating;
    for (auto bin : generating_bins) {
        if (!inputs.count(bin) || !generating.insert(bin).second) {
            throw std::invalid_argument("invalid or duplicate high-order generating bin");
        }
    }
    const auto fundamental = amplifier.fundamental_response(power);
    const auto limiter = amplifier.limiter_response(power);
    const double radius = std::sqrt(power);
    std::vector<Complex> outgoing(n);
    for (const auto &input : inputs) {
        const auto row = outputs.at(input.first), column = input.second;
        outgoing[row] = fundamental.amplitude_gain * incident[column];
        direct(row, column) = fundamental.amplitude_gain;
        if (power > 0.) {
            const auto common =
                .5 * fundamental.radial_gain_difference * (incident[column] / radius);
            for (const auto &driving : inputs) {
                const auto j = driving.second;
                direct(row, j) += common * std::conj(incident[j] / radius);
                conjugate(row, j) += common * (incident[j] / radius);
            }
        }
    }
    const MemorylessPolynomial polynomial(amplifier.voltage_coefficients());
    if (!generating.empty()) {
        const auto required = polynomial_output_bins(generating_bins, polynomial);
        for (auto bin : required) {
            if (bin > 0 && !outputs.count(bin)) {
                throw std::invalid_argument("missing high-order generated output channel");
            }
        }
        // The internal polynomial includes DC for exact closure; RF projection
        // intentionally discards its output and derivative before network assembly.
        std::vector<ConversionChannel> local_channels;
        std::vector<Complex> local_incident;
        std::map<std::size_t, std::size_t> columns;
        for (auto bin : generating_bins) {
            const auto original = inputs.at(bin);
            columns.emplace(original, local_channels.size());
            local_channels.push_back({0, bin});
            local_incident.push_back(limiter.amplitude_gain * incident[original]);
        }
        const auto first_output = local_channels.size();
        for (auto bin : required) {
            local_channels.push_back({1, bin});
            local_incident.push_back(0.);
        }
        if (!required.empty()) {
            const auto generated = linearize_polynomial_amplifier(
                spacing_hz, local_channels, local_incident, polynomial, 0, 1, reference);
            for (std::size_t i = 0; i < required.size(); ++i) {
                if (required[i] == 0) {
                    continue;
                }
                const auto row = outputs.at(required[i]), local_row = first_output + i;
                outgoing[row] += generated.outgoing[local_row];
                Complex common{};
                if (power > 0.) {
                    for (const auto &entry : columns) {
                        const auto normalized = incident[entry.first] / radius;
                        common +=
                            generated.jacobian.direct()(local_row, entry.second) * normalized +
                            generated.jacobian.conjugate()(local_row, entry.second) *
                                std::conj(normalized);
                    }
                    common *= .5 * limiter.radial_gain_difference;
                }
                for (const auto &input : inputs) {
                    const auto column = input.second;
                    const auto found = columns.find(column);
                    if (found != columns.end()) {
                        direct(row, column) +=
                            limiter.amplitude_gain *
                            generated.jacobian.direct()(local_row, found->second);
                        conjugate(row, column) +=
                            limiter.amplitude_gain *
                            generated.jacobian.conjugate()(local_row, found->second);
                    }
                    if (power > 0.) {
                        direct(row, column) += common * std::conj(incident[column] / radius);
                        conjugate(row, column) += common * (incident[column] / radius);
                    }
                }
            }
        }
    }
    for (auto wave : outgoing) {
        if (!conversion_detail::finite(wave)) {
            throw std::overflow_error("high-order output overflow");
        }
    }
    return {FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference), outgoing};
}
} // namespace rfmodel
