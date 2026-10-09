#pragma once
#include "sweep.hpp"
#include "power_wave_reference.hpp"

namespace rfmodel {
// Shapes: frequencies[M], scattering[M][N,N], port_impedances[M][N].
// Noise is empty when not requested, otherwise has shape [M][N,N] in W/Hz.
struct LinearAnalysisResult {
    std::vector<double> frequencies_hz;
    std::vector<SMatrix> scattering;
    std::vector<std::vector<Complex>> port_impedances_ohms;
    std::vector<std::size_t> external_ports;
    std::vector<NoiseCorrelation> noise_correlation;
};

inline LinearAnalysisResult analyze_linear(
    const FrequencyGrid &grid,
    const std::vector<std::size_t> &external_ports,
    const std::function<LinearNetwork(double)> &build,
    const std::function<NoiseCorrelation(double, const LinearNetwork &)> &intrinsic_noise = {},
    const std::function<std::vector<Complex>(double)> &output_references = {}) {
    if (grid.hz.empty() || external_ports.empty() || !build) {
        throw std::invalid_argument("empty linear analysis request");
    }
    for (std::size_t i = 0; i < grid.hz.size(); ++i) {
        if (!std::isfinite(grid.hz[i]) || grid.hz[i] < 0 || (i && grid.hz[i] <= grid.hz[i - 1])) {
            throw std::invalid_argument("invalid linear analysis frequency grid");
        }
    }
    LinearAnalysisResult result;
    result.external_ports = external_ports;
    for (double f : grid.hz) {
        try {
            auto network = build(f);
            auto matrix = network.external_s(external_ports);
            std::vector<Complex> references(external_ports.size(),
                                            network.reference_impedance_ohms());
            NoiseCorrelation noise;
            if (intrinsic_noise) {
                noise = network.external_noise(external_ports, intrinsic_noise(f, network));
            }
            if (output_references) {
                auto requested = output_references(f);
                const auto transform = renormalize_power_waves(matrix, references, requested);
                if (intrinsic_noise) {
                    noise = propagate_noise(transform.noise_transfer, noise);
                }
                matrix = transform.scattering;
                references = std::move(requested);
            }
            if (intrinsic_noise) {
                result.noise_correlation.push_back(std::move(noise));
            }
            result.frequencies_hz.push_back(f);
            result.scattering.push_back(std::move(matrix));
            result.port_impedances_ohms.push_back(std::move(references));
        } catch (const std::exception &error) {
            throw std::runtime_error("linear analysis failed at " + std::to_string(f) +
                                     " Hz: " + error.what());
        }
    }
    return result;
}
} // namespace rfmodel
