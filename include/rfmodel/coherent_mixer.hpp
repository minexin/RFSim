#pragma once
#include "coherence.hpp"
#include "ideal_mixer.hpp"
#include <algorithm>
#include <cstdlib>

namespace rfmodel {
// One RF component on one branch, driven by a prescribed noiseless CW LO.
// All RF and LO groups belong to the same analysis-local namespace.
struct CoherentMixerInput {
    CoherentComponent component;
    int lo_bin{};
    double conversion_gain_db{};
    double lo_phase_radians{};
    std::uint64_t lo_coherence_group{};
};

// Batch all parallel branches so equal (RF group, LO group) pairs share IDs.
// Output order is [difference, sum] for each input, including zero amplitudes.
// No merging occurs here: route these waves through the following network first.
// reserved_group_max reserves IDs of other, bypassing components in the context.
inline std::vector<CoherentComponent>
mix_coherent_components(double spacing_hz,
                        const std::vector<CoherentMixerInput> &input,
                        std::uint64_t reserved_group_max = 0) {
    if (input.size() > 2048) {
        throw std::invalid_argument("coherent mixer accepts at most 2048 inputs");
    }
    reduce_coherent_components(spacing_hz, {});
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    std::map<Key, std::uint64_t> groups;
    std::uint64_t highest = reserved_group_max;
    std::vector<CoherentComponent> output;
    output.reserve(2 * input.size());
    for (const auto &entry : input) {
        const auto &component = entry.component;
        // Validate each branch independently, without merging before conversion.
        reduce_coherent_components(spacing_hz, {component});
        if (entry.lo_coherence_group == 0 || entry.lo_bin == component.bin) {
            throw std::invalid_argument("coherent RF mixer requires an LO group and nonzero IF");
        }
        IdealRealMixer mixer(
            "coherent mixer", entry.lo_bin, entry.conversion_gain_db, entry.lo_phase_radians);
        const auto converted = mixer.transmit({spacing_hz, {{component.bin, component.amplitude}}});
        const auto difference =
            static_cast<int>(std::abs(static_cast<long long>(component.bin) - entry.lo_bin));
        const auto sum = static_cast<long long>(component.bin) + entry.lo_bin;
        // Check even zero-amplitude inputs, which the scalar mixer may skip.
        if (sum > std::numeric_limits<int>::max()) {
            throw std::overflow_error("coherent mixer frequency index overflow");
        }
        for (int bin : {difference, static_cast<int>(sum)}) {
            auto value = component;
            value.bin = bin;
            const auto wave = converted.amplitudes.find(bin);
            value.amplitude = wave == converted.amplitudes.end() ? Complex{} : wave->second;
            reduce_coherent_components(spacing_hz, {value});
            output.push_back(value);
        }
        groups.emplace(Key{component.coherence_group, entry.lo_coherence_group}, 0);
        highest = std::max({highest, component.coherence_group, entry.lo_coherence_group});
    }
    if (groups.size() > std::numeric_limits<std::uint64_t>::max() - highest) {
        throw std::overflow_error("coherent mixer group namespace exhausted");
    }
    for (auto &group : groups) {
        group.second = ++highest;
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto group =
            groups.at({input[i].component.coherence_group, input[i].lo_coherence_group});
        output[2 * i].coherence_group = group;
        output[2 * i + 1].coherence_group = group;
    }
    return output;
}
} // namespace rfmodel
