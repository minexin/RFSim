#include <rfmodel/coherent_pipeline.hpp>
#include <cmath>
#include <stdexcept>

// Independently instantiate all five operations in a second translation unit.
// Checks use return values, not assert(), so Release/NDEBUG keeps them active.
bool coherent_pipeline_odr_probe() {
    using namespace rfmodel;
    CoherentPipeline pipeline(1e8, 50., 2);
    const auto source = pipeline.seed({{8, SpectrumKind::source, 1., 1, {.01, 0.}},
                                       {12, SpectrumKind::source, 1., 1, {-.01, 0.}}});
    const auto mixed = pipeline.mix(source, {10, 2, 0., 0.});
    const auto filtered = pipeline.linear_network({{mixed, 0}}, {0, 1}, 1, [](double frequency) {
        const double transfer = frequency == 2e8 ? 1. : 0.;
        LinearNetwork network(50.);
        network.add({2, {0., transfer, transfer, 0.}});
        return network;
    });
    const auto compressed =
        pipeline.compress_fundamentals(filtered, SaturatingFundamentalCompression(20., 20., 23.));
    if (compressed.input_power_w != 0. || compressed.common_amplitude_gain != 10.) {
        return false;
    }
    bool retained_cancellation = false;
    for (const auto &record : pipeline.inspect(compressed.output).records) {
        if (record.component.bin == 2) {
            if (record.component.amplitude != Complex{} || record.history.size() != 2) {
                return false;
            }
            for (const auto &term : record.history) {
                if (std::abs(std::abs(term.amplitude) - .1) > 1e-16 || term.factors.size() != 2) {
                    return false;
                }
            }
            retained_cancellation = true;
        }
    }
    const auto quadratic = pipeline.polynomial(filtered, {0., 0., .5}, 4);
    bool retained_harmonic = false;
    for (const auto &record : pipeline.inspect(quadratic.output).records) {
        if (record.component.bin == 4) {
            if (record.component.kind != SpectrumKind::harmonic ||
                std::abs(record.component.amplitude) > 1e-18 || record.history.size() != 3) {
                return false;
            }
            for (const auto &term : record.history) {
                if (term.factors.size() != 4 || term.amplitude == Complex{}) {
                    return false;
                }
            }
            retained_harmonic = true;
        }
    }
    CoherentPipeline unrelated(1e8, 50., 2);
    bool rejected_foreign_handle = false;
    try {
        unrelated.inspect(filtered);
    } catch (const std::invalid_argument &) {
        rejected_foreign_handle = true;
    }
    return retained_cancellation && retained_harmonic && rejected_foreign_handle;
}
