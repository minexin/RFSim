#include "rfmodel/coherent_compression.hpp"
#include "test_support.hpp"
#include <algorithm>

int main() {
    using namespace rfmodel;
    const SaturatingFundamentalCompression model(20., 20., 23.);
    const double anchor = model.input_p1db_watts();
    const double wave = std::sqrt(anchor / 2);
    std::vector<CoherentComponent> input{{10, SpectrumKind::source, 1., 1, wave},
                                         {10, SpectrumKind::source, 1., 2, Complex{0., wave}}};
    // Independent carriers share one gain law, without inventing a common phase.
    const auto at_p1 = compress_coherent_fundamentals(1e8, input, model);
    near(at_p1.input_power_w, anchor);
    near(at_p1.output.total_power_w, .1);
    require(at_p1.output.components.size() == 2, "retain independent groups");
    near(at_p1.output.components[0].amplitude, std::sqrt(.05));
    near(at_p1.output.components[1].amplitude, Complex{0., std::sqrt(.05)});
    input[1].bin = 11;
    const auto two_tone = compress_coherent_fundamentals(1e8, input, model);
    near(two_tone.output.power_by_bin_w.at(10), .05);
    near(two_tone.output.power_by_bin_w.at(11), .05);
    // Output power remains bounded by a single saturation budget across all bins.
    for (auto &component : input) {
        component.amplitude *= 1e4;
    }
    const auto saturated = compress_coherent_fundamentals(1e8, input, model);
    near(saturated.output.total_power_w, std::pow(10., -.7));
    near(saturated.output.power_by_bin_w.at(10), saturated.output.power_by_bin_w.at(11));
    // A cancelled coherent pair does not compress another surviving carrier.
    input = {{10, SpectrumKind::source, 1., 1, 1.},
             {10, SpectrumKind::source, 1., 1, -1.},
             {11, SpectrumKind::source, 1., 2, .001}};
    const auto cancelled = compress_coherent_fundamentals(1e8, input, model);
    near(cancelled.input_power_w, 1e-6);
    near(cancelled.output.components[0].amplitude, 0.);
    near(cancelled.output.components[1].amplitude, model.transmit_fundamental(.001));
    std::reverse(input.begin(), input.end());
    const auto reordered = compress_coherent_fundamentals(1e8, input, model);
    near(reordered.output.total_power_w, cancelled.output.total_power_w);
    require(reordered.output.components[1].coherence_group == 2, "stable identities");
    // Same frequency/clock with distinct bandwidths must not cancel.
    input = {{10, SpectrumKind::source, 1., 1, wave}, {10, SpectrumKind::source, 2., 1, -wave}};
    near(compress_coherent_fundamentals(1e8, input, model).output.total_power_w, .1);
    require(compress_coherent_fundamentals(1e8, {}, model).output.components.empty(),
            "empty input");
    for (auto kind : {SpectrumKind::harmonic, SpectrumKind::intermod}) {
        input[1].kind = kind;
        rejects<std::invalid_argument>([&] {
            compress_coherent_fundamentals(1e8, input, model);
        });
    }
    rejects<std::invalid_argument>([&] {
        compress_coherent_fundamentals(0., {}, model);
    });
    rejects<std::invalid_argument>([&] {
        compress_coherent_fundamentals(1e8, std::vector<CoherentComponent>(4097), model);
    });
    rejects<std::overflow_error>([&] {
        compress_coherent_fundamentals(
            1e8,
            {{10, SpectrumKind::source, 1., 1, 1e154}, {11, SpectrumKind::source, 1., 2, 1e154}},
            model);
    });
}
