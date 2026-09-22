#pragma once
#include "sweep.hpp"
namespace rfmodel {
// Shapes: frequencies[M], scattering[M][N,N], port_impedances[M][N].
// CS is deliberately absent until correlated network noise is implemented.
struct LinearAnalysisResult {
    std::vector<double> frequencies_hz;
    std::vector<SMatrix> scattering;
    std::vector<std::vector<Complex>> port_impedances_ohms;
    std::vector<std::size_t> external_ports;
};
inline LinearAnalysisResult analyze_linear(
    const FrequencyGrid& grid, const std::vector<std::size_t>& external_ports,
    const std::function<LinearNetwork(double)>& build) {
    if (grid.hz.empty() || external_ports.empty() || !build)
        throw std::invalid_argument("empty linear analysis request");
    for (std::size_t i=0; i<grid.hz.size(); ++i)
        if (!std::isfinite(grid.hz[i]) || grid.hz[i]<0 || (i && grid.hz[i]<=grid.hz[i-1]))
            throw std::invalid_argument("invalid linear analysis frequency grid");
    LinearAnalysisResult result;
    result.external_ports=external_ports;
    for (double f : grid.hz) {
        try {
            auto network=build(f);
            auto matrix=network.external_s(external_ports);
            result.frequencies_hz.push_back(f);
            result.scattering.push_back(std::move(matrix));
            result.port_impedances_ohms.emplace_back(external_ports.size(),network.reference_impedance_ohms());
        } catch (const std::exception& error) {
            throw std::runtime_error("linear analysis failed at " + std::to_string(f) + " Hz: " + error.what());
        }
    }
    return result;
}
}
