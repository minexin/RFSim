#pragma once
#include "installed_headers.hpp"

namespace res_probe {
constexpr double frequency_hz = 100e6;
constexpr double reference_ohms = 50.;
constexpr double resistance_ohms = 50.;
constexpr double source_available_w = .001;

// This is a local fixture/orchestrator, not an extra RFModel API.
struct Fixture {
    std::vector<rfmodel::ModelAdapter> models;
    std::vector<rfmodel::ConversionPortConnection> connections;
    std::vector<std::size_t> external_ports;
};
Fixture fixture();
rfmodel::LinearNetwork linear_network(const Fixture &fixture, double frequency);
rfmodel::ConversionSimulationRequest loaded_request(const Fixture &fixture);
} // namespace res_probe
