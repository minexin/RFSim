#include "rfmodel/coherence.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <limits>

int main() {
    using namespace rfmodel;
    const CoherentComponent a{10, SpectrumKind::source, 1., 1, {1., 0.}};
    auto b = a;
    auto result = reduce_coherent_components(1e8, {a, b});
    require(result.components.size() == 1, "same coherence key must merge");
    near(result.components[0].amplitude, 2.);
    near(result.total_power_w, 4.);
    b.amplitude = -1.;
    result = reduce_coherent_components(1e8, {a, b});
    require(result.components.size() == 1 && result.power_by_bin_w.size() == 1,
            "cancellation must preserve the group and bin");
    near(result.total_power_w, 0.);
    for (int difference = 0; difference < 4; ++difference) {
        b = a;
        b.amplitude = -1.;
        if (difference == 0) {
            b.coherence_group = 2;
        }
        if (difference == 1) {
            b.kind = SpectrumKind::intermod;
        }
        if (difference == 2) {
            b.bandwidth_hz = 3.;
        }
        if (difference == 3) {
            b.bin = 11;
        }
        result = reduce_coherent_components(1e8, {a, b});
        require(result.components.size() == 2, "distinct coherence keys must remain separate");
        near(result.total_power_w, 2.);
    }
    auto huge = a;
    huge.amplitude = 1e100;
    auto negative = huge;
    negative.amplitude = -1e100;
    result = reduce_coherent_components(1e8, {huge, a, negative});
    near(result.total_power_w, 1.);
    require(reduce_coherent_components(1e8, {}).components.empty(), "empty reduction");
    for (int mutation = 0; mutation < 7; ++mutation) {
        b = a;
        if (mutation == 0) {
            b.bin = 0;
        }
        if (mutation == 1) {
            b.kind = static_cast<SpectrumKind>(3);
        }
        if (mutation == 2) {
            b.coherence_group = 0;
        }
        if (mutation == 3) {
            b.bandwidth_hz = 0.;
        }
        if (mutation == 4) {
            b.bandwidth_hz = 3e9;
        }
        if (mutation == 5) {
            b.amplitude = std::numeric_limits<double>::infinity();
        }
        if (mutation == 6) {
            b.bandwidth_hz = std::numeric_limits<double>::quiet_NaN();
        }
        rejects<std::invalid_argument>([&] {
            reduce_coherent_components(1e8, {b});
        });
    }
    rejects<std::invalid_argument>([&] {
        reduce_coherent_components(0., {});
    });
    rejects<std::invalid_argument>([&] {
        reduce_coherent_components(1e8, std::vector<CoherentComponent>(4097, a));
    });
    b = a;
    b.amplitude = 1e154;
    rejects<std::overflow_error>([&] {
        reduce_coherent_components(1e8, {b, b});
    });
    auto separate = b;
    separate.coherence_group = 2;
    rejects<std::overflow_error>([&] {
        reduce_coherent_components(1e8, {b, separate});
    });
}
