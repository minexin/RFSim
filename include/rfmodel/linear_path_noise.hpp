#pragma once
#include "linear_path.hpp"

namespace rfmodel {
struct LinearPathNoiseResult {
    SMatrix scattering;
    NoiseCorrelation intrinsic;
    double transducer_gain;
    double source_output_w_per_hz;
    double intrinsic_output_w_per_hz;
    double total_output_w_per_hz;
    // Uses reference_temperature_k, independently of the actual source temperature.
    // Undefined for zero delivered signal gain (including a lossless reflecting load).
    std::optional<double> noise_factor;
};

// One frequency; independent stages, with correlated noise allowed within each stage.
// The source emits k*T available noise; the load is noiseless. All reflection feedback
// is retained. Device physical temperatures are already encoded in stage_noise.
inline LinearPathNoiseResult
analyze_linear_path_noise(const std::vector<LinearPathStage> &stages,
                          const std::vector<NoiseCorrelation> &stage_noise,
                          double source_temperature_k = 290.,
                          Complex source_reflection = {},
                          Complex load_reflection = {},
                          double reference_temperature_k = 290.) {
    if (stages.empty() || stages.size() > 512 || stage_noise.size() != stages.size()) {
        throw std::invalid_argument("path noise requires one noise matrix per two-port stage");
    }
    if (!std::isfinite(source_temperature_k) || source_temperature_k < 0 ||
        !std::isfinite(reference_temperature_k) || reference_temperature_k <= 0) {
        throw std::invalid_argument("invalid path noise temperature");
    }
    gain_detail::mismatch_factor(source_reflection, false);
    const double load_factor = gain_detail::mismatch_factor(load_reflection, true);
    LinearNetwork network(stages.front().reference_ohms);
    std::set<std::string> names;
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto &stage = stages[index];
        if (stage.name.empty() || !names.insert(stage.name).second ||
            stage_noise[index].watts_per_hz.ports != 2) {
            throw std::invalid_argument("invalid path noise stage name or dimensions");
        }
        gain_detail::validate(stage.scattering);
        network.add(stage.scattering, stage.reference_ohms);
        if (index != 0) {
            network.connect(2 * index - 1, 2 * index);
        }
    }
    const std::vector<std::size_t> ports{0, 2 * stages.size() - 1};
    LinearPathNoiseResult result{};
    result.scattering = network.external_s(ports);
    result.intrinsic = network.external_noise(ports, independent_noise(stage_noise));
    result.transducer_gain =
        transducer_power_gain(result.scattering, source_reflection, load_reflection);

    // b = S*Gamma*b + c; solve for the outgoing waves from the aggregate noise.
    const auto &s = result.scattering;
    const SMatrix feedback{2,
                           {1. - s(0, 0) * source_reflection,
                            -s(0, 1) * load_reflection,
                            -s(1, 0) * source_reflection,
                            1. - s(1, 1) * load_reflection}};
    const SMatrix identity{2, {1., 0., 0., 1.}};
    const auto transfer = parameter_detail::solve(feedback, identity, 1.);
    result.intrinsic_output_w_per_hz =
        load_factor * propagate_noise(transfer, result.intrinsic).watts_per_hz(1, 1).real();
    constexpr double boltzmann = 1.380649e-23;
    result.source_output_w_per_hz = boltzmann * source_temperature_k * result.transducer_gain;
    result.total_output_w_per_hz = result.source_output_w_per_hz + result.intrinsic_output_w_per_hz;
    if (!std::isfinite(result.total_output_w_per_hz)) {
        throw std::overflow_error("path output noise overflow");
    }
    if (result.transducer_gain > 0) {
        const double reference_noise = boltzmann * reference_temperature_k * result.transducer_gain;
        if (reference_noise == 0) {
            throw std::underflow_error("path reference noise underflow");
        }
        const double factor = 1. + result.intrinsic_output_w_per_hz / reference_noise;
        if (!std::isfinite(reference_noise) || !std::isfinite(factor)) {
            throw std::overflow_error("path noise factor overflow");
        }
        result.noise_factor = factor;
    }
    return result;
}
} // namespace rfmodel
