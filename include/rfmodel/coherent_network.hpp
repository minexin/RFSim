#pragma once
#include "coherence.hpp"
#include "network.hpp"
#include <functional>

namespace rfmodel {
struct PortCoherentComponent {
    std::size_t input_port{};
    CoherentComponent component;
};

// Matched external incident waves, observed outgoing wave at one external port.
// Input/output ports are global network indices, not positions in external_ports.
// The callback supplies the entire connected linear network at each frequency.
inline CoherentReduction
transmit_coherent_network(double spacing_hz,
                          const std::vector<PortCoherentComponent> &input,
                          const std::vector<std::size_t> &external_ports,
                          std::size_t output_port,
                          double reference_ohms,
                          const std::function<LinearNetwork(double)> &build) {
    if (!build || input.size() > 4096 || external_ports.empty() || external_ports.size() > 1024 ||
        !std::isfinite(reference_ohms) || reference_ohms <= 0) {
        throw std::invalid_argument("invalid coherent network request");
    }
    // Validate the grid even when there are no incident components.
    reduce_coherent_components(spacing_hz, {});
    std::map<std::size_t, std::size_t> columns;
    for (std::size_t i = 0; i < external_ports.size(); ++i) {
        if (!columns.emplace(external_ports[i], i).second) {
            throw std::invalid_argument("duplicate coherent network external port");
        }
    }
    const auto output = columns.find(output_port);
    if (output == columns.end()) {
        throw std::invalid_argument("output must be a selected external port");
    }
    std::map<std::size_t, std::vector<CoherentComponent>> by_port;
    for (const auto &value : input) {
        if (columns.find(value.input_port) == columns.end()) {
            throw std::invalid_argument("input must be a selected external port");
        }
        by_port[value.input_port].push_back(value.component);
    }
    // Merge repeated coherent contributions on each input port before transfer.
    std::map<int, std::vector<PortCoherentComponent>> by_bin;
    for (const auto &port : by_port) {
        const auto reduced = reduce_coherent_components(spacing_hz, port.second);
        for (const auto &component : reduced.components) {
            by_bin[component.bin].push_back({port.first, component});
        }
    }
    if (by_bin.size() > 2048) {
        throw std::length_error("coherent network frequency limit exceeded");
    }
    auto matrix_at = [&](double frequency) {
        auto network = build(frequency);
        if (network.reference_impedance_ohms() != reference_ohms) {
            throw std::invalid_argument("coherent network reference differs");
        }
        return network.external_s(external_ports);
    };
    if (by_bin.empty()) {
        matrix_at(0.);
    }
    std::vector<CoherentComponent> transmitted;
    for (const auto &entry : by_bin) {
        const auto matrix = matrix_at(entry.first * spacing_hz);
        for (const auto &incident : entry.second) {
            auto component = incident.component;
            component.amplitude *= matrix(output->second, columns.at(incident.input_port));
            if (!std::isfinite(std::norm(component.amplitude))) {
                throw std::overflow_error("coherent network component power overflow");
            }
            transmitted.push_back(component);
        }
    }
    return reduce_coherent_components(spacing_hz, transmitted);
}
} // namespace rfmodel
