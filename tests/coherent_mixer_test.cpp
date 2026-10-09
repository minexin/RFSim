#include "rfmodel/coherent_mixer.hpp"
#include "rfmodel/coherent_network.hpp"
#include "test_support.hpp"
#include <algorithm>

int main() {
    using namespace rfmodel;
    const double half_pi = std::acos(-1.) / 2;
    CoherentComponent rf{10, SpectrumKind::source, 1., 7, 1.};
    // Quadrature RF and LO paths: reject the sum sideband, reinforce difference.
    std::vector<CoherentMixerInput> input{{rf, 8, 0., 0., 9}, {rf, 8, 0., half_pi, 9}};
    input[1].component.amplitude = Complex{0., 1.};
    const auto output = mix_coherent_components(1e8, input, 100);
    require(output.size() == 4 && output[0].bin == 2 && output[1].bin == 18,
            "branch order and sidebands");
    near(output[0].amplitude, 1.);
    near(output[1].amplitude, 1.);
    near(output[2].amplitude, 1.);
    near(output[3].amplitude, -1.);
    require(output[0].coherence_group == output[3].coherence_group &&
                output[0].coherence_group > 100,
            "shared RF and LO, reserved namespace");
    // A physical matched 3-port combiner after frequency conversion.
    auto combine = [](const std::vector<CoherentComponent> &waves) {
        std::vector<PortCoherentComponent> ports;
        for (std::size_t i = 0; i < waves.size(); ++i) {
            ports.push_back({i / 2, waves[i]});
        }
        return transmit_coherent_network(1e8, ports, {2, 0, 1}, 2, 50., [](double) {
            LinearNetwork network;
            const double k = 1 / std::sqrt(2.);
            network.add({3, {0., 0., k, 0., 0., k, k, k, 0.}});
            return network;
        });
    };
    const auto rejected = combine(output);
    near(rejected.power_by_bin_w.at(2), 2.);
    near(rejected.power_by_bin_w.at(18), 0.);
    input[1].lo_coherence_group = 10;
    const auto independent = mix_coherent_components(1e8, input);
    near(combine(independent).power_by_bin_w.at(2), 1.);
    near(combine(independent).power_by_bin_w.at(18), 1.);
    input[1].lo_coherence_group = 9;
    input[1].component.coherence_group = 8;
    near(combine(mix_coherent_components(1e8, input)).power_by_bin_w.at(18), 1.);
    // Group allocation depends on identities, not branch traversal.
    const auto original = mix_coherent_components(1e8, input);
    std::reverse(input.begin(), input.end());
    const auto reversed = mix_coherent_components(1e8, input);
    require(original[0].coherence_group == reversed[2].coherence_group &&
                original[2].coherence_group == reversed[0].coherence_group,
            "stable IDs");
    // Positive-frequency folding conjugates RF, with the positive LO phase.
    rf.bin = 3;
    rf.amplitude = Complex{0., 1.};
    const auto folded = mix_coherent_components(1., {{rf, 8, -6.020599913279624, half_pi, 9}});
    near(folded[0].amplitude, .5);
    near(folded[1].amplitude, -.5);
    require(folded[0].bin == 5 && folded[1].bin == 11, "folded frequencies");
    // Opposite RF images of a shared reference can meet at one IF.
    const auto images =
        mix_coherent_components(1.,
                                {{{1, SpectrumKind::source, 1., 7, 1.}, 2, 0., 0., 9},
                                 {{3, SpectrumKind::source, 1., 7, -1.}, 2, 0., 0., 9}});
    near(reduce_coherent_components(1., images).power_by_bin_w.at(1), 0.);
    auto typed = input;
    typed[1].component.kind = SpectrumKind::harmonic;
    typed[1].component.bandwidth_hz = 2.;
    const auto retained = mix_coherent_components(1e8, typed);
    require(retained[2].kind == SpectrumKind::harmonic && retained[2].bandwidth_hz == 2.,
            "conversion preserves kind and bandwidth");
    rf.amplitude = 0.;
    const auto zeros = mix_coherent_components(1., {{rf, 8, 0., 0., 9}});
    require(zeros.size() == 2, "retain zero branches");
    require(mix_coherent_components(1., {}).empty(), "empty batch");
    for (const auto &bad : std::vector<CoherentMixerInput>{
             {rf, 3, 0., 0., 9},
             {rf, 0, 0., 0., 9},
             {rf, 8, 0., 0., 0},
             {rf, 8, 0., std::numeric_limits<double>::quiet_NaN(), 9}}) {
        rejects<std::invalid_argument>([&] {
            mix_coherent_components(1., {bad});
        });
    }
    rf.bandwidth_hz = 5.;
    rejects<std::invalid_argument>([&] {
        mix_coherent_components(1., {{rf, 4, 0., 0., 9}});
    });
    rf.bandwidth_hz = 1.;
    rejects<std::overflow_error>([&] {
        mix_coherent_components(
            1., {{rf, 8, 0., 0., 9}}, std::numeric_limits<std::uint64_t>::max());
    });
    rf.bin = std::numeric_limits<int>::max();
    rejects<std::overflow_error>([&] {
        mix_coherent_components(1., {{rf, 8, 0., 0., 9}});
    });
    rf.bin = 3;
    rf.amplitude = 1e150;
    rejects<std::invalid_argument>([&] {
        mix_coherent_components(1., {{rf, 8, 300., 0., 9}});
    });
    rejects<std::invalid_argument>([] {
        mix_coherent_components(1., std::vector<CoherentMixerInput>(2049));
    });
    rejects<std::invalid_argument>([] {
        mix_coherent_components(0., {});
    });
}
