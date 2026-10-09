#include "rfmodel/coherent_network.hpp"
#include "rfmodel/saturating_fundamental.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const CoherentComponent source{10, SpectrumKind::source, 1., 7, 1.};
    std::vector<PortCoherentComponent> inputs{{0, source}, {1, source}};
    std::map<double, int> calls;
    auto combiner = [&](double frequency) {
        ++calls[frequency];
        const double k = 1. / std::sqrt(2.);
        LinearNetwork n;
        n.add({3, {0., 0., k, 0., 0., k, k, k, 0.}});
        return n;
    };
    auto result = transmit_coherent_network(1e8, inputs, {2, 1, 0}, 2, 50., combiner);
    near(result.total_power_w, 2.);
    require(calls[1e9] == 1, "one matrix extraction per frequency");
    inputs[1].component.amplitude = -1.;
    result = transmit_coherent_network(1e8, inputs, {0, 1, 2}, 2, 50., combiner);
    near(result.total_power_w, 0.);
    require(result.components.size() == 1, "preserve cancelled group");
    const SaturatingFundamentalCompression amplifier(20., 20., 23.);
    near(amplifier.transmit_fundamental(result.components[0].amplitude, result.total_power_w), 0.);
    inputs[1].component.coherence_group = 8;
    result = transmit_coherent_network(1e8, inputs, {0, 1, 2}, 2, 50., combiner);
    near(result.total_power_w, 1.);
    require(result.components.size() == 2, "independent sources must not cancel");
    // Internal mismatched cascade, including infinite reflected paths: .5*.4/(1-.2*.3).
    auto feedback = [](double) {
        LinearNetwork n;
        n.add({2, {0., .5, .5, .2}});
        n.add({2, {.3, .4, .4, 0.}});
        n.connect(1, 2);
        return n;
    };
    result = transmit_coherent_network(1e8, {{0, source}}, {0, 3}, 3, 50., feedback);
    near(result.components[0].amplitude, .2 / .94);
    // Split one source into two arms, delay one arm, then recombine.
    auto delay = [](double frequency) {
        LinearNetwork n;
        const double k = 1. / std::sqrt(2.);
        const Complex phase = std::polar(1., -2 * 3.141592653589793 * frequency * .5e-9);
        n.add({3, {0., k, k, k, 0., 0., k, 0., 0.}});
        n.add({2, {0., 1., 1., 0.}});
        n.add({2, {0., phase, phase, 0.}});
        n.add({3, {0., k, k, k, 0., 0., k, 0., 0.}});
        n.connect(1, 3);
        n.connect(2, 5);
        n.connect(4, 8);
        n.connect(6, 9);
        return n;
    };
    auto second = source;
    second.bin = 20;
    result = transmit_coherent_network(1e8, {{0, source}, {0, second}}, {0, 7}, 7, 50., delay);
    near(result.power_by_bin_w.at(10), 0.);
    near(result.power_by_bin_w.at(20), 1.);
    inputs = {{0, source}, {1, source}};
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto bad = inputs;
        auto ports = std::vector<std::size_t>{0, 1, 2};
        std::size_t output = 2;
        if (mutation == 0) {
            bad[0].input_port = 99;
        }
        if (mutation == 1) {
            ports = {0, 0, 2};
        }
        if (mutation == 2) {
            output = 99;
        }
        if (mutation == 3) {
            bad[0].component.bandwidth_hz = 0.;
        }
        if (mutation == 4) {
            bad[0].component.amplitude = std::numeric_limits<double>::infinity();
        }
        rejects<std::invalid_argument>([&] {
            transmit_coherent_network(1e8, bad, ports, output, 50., combiner);
        });
    }
    rejects<std::exception>([&] {
        transmit_coherent_network(1e8, {}, {0, 99}, 0, 50., feedback);
    });
    rejects<std::invalid_argument>([&] {
        transmit_coherent_network(1e8, {{0, source}}, {0, 3}, 3, 75., feedback);
    });
    rejects<std::invalid_argument>([&] {
        transmit_coherent_network(0., {}, {0, 1, 2}, 2, 50., combiner);
    });
    rejects<std::invalid_argument>([&] {
        transmit_coherent_network(1e8, {}, {0, 1, 2}, 2, 50., {});
    });
    rejects<std::invalid_argument>([&] {
        transmit_coherent_network(1e8,
                                  std::vector<PortCoherentComponent>(4097, {0, source}),
                                  {0, 1, 2},
                                  2,
                                  50.,
                                  combiner);
    });
    std::vector<PortCoherentComponent> many_frequencies;
    for (int bin = 1; bin <= 2049; ++bin) {
        auto component = source;
        component.bin = bin;
        many_frequencies.push_back({0, component});
    }
    calls.clear();
    rejects<std::length_error>([&] {
        transmit_coherent_network(1e8, many_frequencies, {0, 1, 2}, 2, 50., combiner);
    });
    require(calls.empty(), "resource limits checked before building networks");
    auto gain = [](double) {
        LinearNetwork n;
        n.add({2, {0., 0., 2., 0.}});
        return n;
    };
    auto huge = source;
    huge.amplitude = 1e154;
    rejects<std::overflow_error>([&] {
        transmit_coherent_network(1e8, {{0, huge}}, {0, 1}, 1, 50., gain);
    });
    require(transmit_coherent_network(1e8, {}, {0, 1, 2}, 2, 50., combiner).components.empty(),
            "empty still validates topology");
}
