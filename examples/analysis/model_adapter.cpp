#include <rfmodel/adapters/builtin_model_adapters.hpp>
#include <rfmodel/models/ideal_devices.hpp>
#include <iostream>

int main() {
    using namespace rfmodel;
    // The host owns type identifiers and parameter conversion. The network sees S only.
    auto pad = adapt_passive_model(
        MatchedTransmissionModel("pad", 20. * std::log10(2.)), {"example.pad", "1", "", ""}, 290.);
    LinearNetwork network;
    pad.add_to(network, 1e9);
    const auto scattering = network.external_s({0, 1});
    const auto noise = network.external_noise({0, 1}, pad.noise_correlation(1e9));
    auto sampled = pad.conversion_device(1e8, {8, 12}, ModelNoisePolicy::require_model_noise);
    sampled.source[2] = 1.; // RF at bin 12, physical input port 0.
    FrequencyConversionNetwork converter(1e8);
    converter.add(sampled);
    converter.add(ideal_mixer_conversion(1e8, {{0, 8}, {0, 12}, {1, 2}, {1, 18}, {1, 22}}, 10, 0.));
    converter.connect(0, 1, 1, 0);
    const auto converted = converter.analyze();
    auto amplifier = adapt_polynomial_amplifier(MatchedPolynomialAmplifier("PA", {0., 2., 0., -.1}),
                                                {"example.polynomial", "1", "", ""});
    const auto capabilities = amplifier.capabilities();
    if (!capabilities.coherent || capabilities.noise) {
        return 1;
    }
    CoherentSystem graph(1e8, 50., {{"rf", "clock"}});
    auto input = graph.input("rf", {{10, SpectrumKind::source, 1., graph.source_group("rf"), .02}});
    const auto output = amplifier.coherent_stage(graph, "PA", {input}, {"amplified"}).front();
    // A host extension composes provenance-preserving operations without editing the engine.
    graph.stage("host_stage", {output}, {"half"}, [](auto &pipeline, const auto &inputs) {
        const auto half = pipeline.polynomial(inputs.at(0), {0., .5}, 11);
        return CoherentSystem::StageEvaluation{{half.output}, {}};
    });
    const auto result = graph.compile().execute();
    const auto before = result.stream(output).reduction.total_power_w;
    const auto after = result.stream("half").reduction.total_power_w;
    if (std::abs(scattering(1, 0) - Complex{.5, 0.}) > 1e-14 ||
        std::abs(after - .25 * before) > 1e-14) {
        return 1;
    }
    std::cout << "S21=" << scattering(1, 0) << ", noise=" << noise.watts_per_hz(1, 1)
              << " W/Hz, custom-stage output=" << after << " W, IF=" << converted.outgoing[6]
              << ", IF intrinsic noise=" << converted.outgoing_noise.covariance(6, 6) << " W/Hz\n";
    return 0;
}
