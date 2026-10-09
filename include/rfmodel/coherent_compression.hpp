#pragma once
#include "coherence.hpp"
#include "saturating_fundamental.hpp"

namespace rfmodel {
struct CoherentCompressionResult {
    double input_power_w{};
    CoherentReduction output;
};

// Matched, forward-only shared compression of supplied carrier groups.
// Merge coherent paths before evaluating drive; retain independent identities.
// No new harmonics, intermods, AM/PM, noise or reverse-wave feedback.
inline CoherentCompressionResult
compress_coherent_fundamentals(double spacing_hz,
                               const std::vector<CoherentComponent> &input,
                               const SaturatingFundamentalCompression &model) {
    auto reduced = reduce_coherent_components(spacing_hz, input);
    const double drive = reduced.total_power_w;
    for (auto &component : reduced.components) {
        if (component.kind != SpectrumKind::source) {
            throw std::invalid_argument(
                "fundamental compression accepts source-kind carriers only");
        }
        component.amplitude = model.transmit_fundamental(component.amplitude, drive);
    }
    return {drive, reduce_coherent_components(spacing_hz, reduced.components)};
}
} // namespace rfmodel
