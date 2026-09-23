#pragma once
#include "measurements.hpp"
#include "network.hpp"
#include "power_gain.hpp"
#include <optional>
#include <set>

namespace rfmodel {
struct LinearPathStage {
    std::string name;
    SMatrix scattering;
    double reference_ohms = 50;
};

struct LinearPathStageResult {
    std::string name;
    PortPower input;
    PortPower output;
    // Undefined when net input power is nonpositive or net output power is negative.
    std::optional<double> operating_gain;
    std::optional<double> cumulative_operating_gain;
};

struct LinearPathResult {
    std::vector<LinearPathStageResult> stages;
    NetworkWaves waves;
    double source_available_w;
    double input_accepted_w;
    double load_delivered_w;
    double input_power_fraction;
    std::optional<double> transducer_gain;
};

namespace path_detail {
inline std::optional<double> power_ratio(double delivered, double accepted) {
    if (accepted <= 0 || delivered < 0) {
        return std::nullopt;
    }
    const double ratio = delivered / accepted;
    if (!std::isfinite(ratio)) {
        throw std::overflow_error("linear path gain overflow");
    }
    return ratio;
}
} // namespace path_detail

// One frequency point, ordered two-port stages: output i connects to input i+1.
// Solve the whole network together so that reverse transmission and reflections
// affect every stage. No unilateral cascade or matched-stage approximation.
inline LinearPathResult analyze_linear_path(const std::vector<LinearPathStage> &stages,
                                            double source_available_w,
                                            Complex source_reflection = {},
                                            Complex load_reflection = {}) {
    if (stages.empty() || stages.size() > 512) {
        throw std::invalid_argument("linear path requires 1 to 512 two-port stages");
    }
    if (!std::isfinite(source_available_w) || source_available_w <= 0) {
        throw std::invalid_argument("linear path requires positive finite source power");
    }
    const double source_factor = gain_detail::mismatch_factor(source_reflection, false);
    gain_detail::mismatch_factor(load_reflection, true);
    LinearNetwork network(stages.front().reference_ohms);
    std::set<std::string> names;
    for (const auto &stage : stages) {
        if (stage.name.empty() || !names.insert(stage.name).second) {
            throw std::invalid_argument("linear path stage names must be nonempty and unique");
        }
        gain_detail::validate(stage.scattering);
        network.add(stage.scattering, stage.reference_ohms);
    }
    for (std::size_t stage = 1; stage < stages.size(); ++stage) {
        network.connect(2 * stage - 1, 2 * stage);
    }
    const double excitation = std::sqrt(source_available_w) * std::sqrt(source_factor);
    if (excitation == 0) {
        throw std::underflow_error("linear path source excitation underflow");
    }
    network.terminate(0, source_reflection, excitation);
    network.terminate(2 * stages.size() - 1, load_reflection);

    LinearPathResult result{};
    result.waves = network.solve();
    result.source_available_w = source_available_w;
    result.input_accepted_w =
        port_power(result.waves.incident.front(), result.waves.outgoing.front()).absorbed_w;
    result.load_delivered_w =
        -port_power(result.waves.incident.back(), result.waves.outgoing.back()).absorbed_w;
    result.input_power_fraction = result.input_accepted_w / source_available_w;
    if (!std::isfinite(result.input_power_fraction)) {
        throw std::overflow_error("linear path input power fraction overflow");
    }
    result.transducer_gain = path_detail::power_ratio(result.load_delivered_w, source_available_w);
    for (std::size_t stage = 0; stage < stages.size(); ++stage) {
        const std::size_t input_port = 2 * stage;
        const std::size_t output_port = input_port + 1;
        const auto input =
            port_power(result.waves.incident[input_port], result.waves.outgoing[input_port]);
        const auto output =
            port_power(result.waves.incident[output_port], result.waves.outgoing[output_port]);
        result.stages.push_back(
            {stages[stage].name,
             input,
             output,
             path_detail::power_ratio(-output.absorbed_w, input.absorbed_w),
             path_detail::power_ratio(-output.absorbed_w, result.input_accepted_w)});
    }
    return result;
}
} // namespace rfmodel
