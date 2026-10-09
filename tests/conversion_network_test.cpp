#include "rfmodel/conversion_network.hpp"
#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    const std::vector<int> bins{8, 12};
    MatchedTransmissionModel pad("pad", 20. * std::log10(2.), 0.);
    std::vector<SMatrix> samples;
    std::vector<NoiseCorrelation> noise;
    for (int bin : bins) {
        auto s = pad.s_parameters(bin * 1e8);
        samples.push_back(s);
        noise.push_back(passive_thermal_noise(s, 290.));
    }
    auto input = lift_linear_conversion(1e8, bins, samples, noise);
    input.source[2] = 1.;
    input.source_noise.covariance(0, 0) = input.source_noise.covariance(2, 2) = 1e-20;
    const std::vector<ConversionChannel> channels{{0, 8}, {0, 12}, {1, 2}, {1, 18}, {1, 22}};
    auto mixer = ideal_mixer_conversion(1e8, channels, 10, 0.);
    FrequencyConversionNetwork network(1e8);
    network.add(input);
    network.add(mixer);
    network.connect(0, 1, 1, 0);
    const auto result = network.analyze();
    near(result.outgoing[6], .5);
    near(result.incident[4], result.outgoing[1]);
    near(result.incident[1], result.outgoing[4]);
    constexpr double thermal = 1.380649e-23 * 290.;
    near(result.outgoing_noise.covariance(6, 6) / 1e-20, 2. * (.25 + .75 * thermal / 1e-20));
    // Ordinary same-frequency reciprocal networks retain exact feedback behavior.
    FrequencyConversionNetwork conversion(1e9);
    SMatrix first{2, {.1, .7, .7, -.2}}, second{2, {.3, .6, .6, -.1}};
    auto a = lift_linear_conversion(1e9, {1}, {first}, {{conversion_detail::zero(2)}});
    a.source[0] = {1., .2};
    a.reflection[0] = .25;
    auto b = lift_linear_conversion(1e9, {1}, {second}, {{conversion_detail::zero(2)}});
    b.reflection[1] = {-.2, .1};
    conversion.add(a);
    conversion.add(b);
    conversion.connect(0, 1, 1, 0);
    LinearNetwork ordinary;
    ordinary.add(first);
    ordinary.add(second);
    ordinary.connect(1, 2);
    ordinary.terminate(0, .25, {1., .2});
    ordinary.terminate(3, {-.2, .1});
    const auto expected = ordinary.solve();
    const auto actual = conversion.analyze();
    for (std::size_t i = 0; i < 4; ++i) {
        near(actual.incident[i], expected.incident[i]);
        near(actual.outgoing[i], expected.outgoing[i]);
    }
    rejects<std::invalid_argument>([&] {
        network.connect(0, 1, 1, 0);
    });
    FrequencyConversionNetwork wrong(1e8);
    wrong.add(input);
    wrong.add(mixer);
    rejects<std::invalid_argument>([&] {
        wrong.connect(0, 1, 1, 1);
    });
    wrong.connect(0, 1, 1, 0); // Failed physical connection did not claim any bins.
    wrong.analyze();
    FrequencyConversionNetwork driven(1e8);
    input.source[1] = 1.;
    driven.add(input);
    driven.add(mixer);
    driven.connect(0, 1, 1, 0);
    rejects<std::invalid_argument>([&] {
        driven.analyze();
    });
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
