#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "rfmodel/models/ideal_devices.hpp"
#include "rfmodel/io/tabulated_noise_model.hpp"
#include "rfmodel/conversion/output_phase_linearization.hpp"
#include "rfmodel/nonlinear/ideal_mixer.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
ModelDescriptor one_port() {
    return {{"example.feedback", "1", "", ""}, "custom", {{0, "port", 50.}}};
}

void linear_noise_and_ownership() {
    auto adapter = [] {
        MatchedTransmissionModel pad("pad", 20. * std::log10(2.));
        return adapt_device_model(pad, {"example.pad", "1", "", ""});
    }();
    const auto caps = adapter.capabilities();
    require(caps.linear && !caps.noise && !caps.spectrum && !caps.operating_point && !caps.coherent,
            "capabilities must reflect implemented hooks");
    LinearNetwork network;
    adapter.add_to(network, 1e9);
    network.terminate(0, .2, 1.);
    network.terminate(1, .3);
    const auto waves = network.solve();
    near(waves.outgoing[1], .5 / (1. - .2 * .3 * .25));
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.noise_correlation(1e9);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.spectrum_stage("unavailable");
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.operating_device(0, 1., {{0, 1}});
    });

    auto passive = adapt_passive_model(MatchedTransmissionModel("pad", 20. * std::log10(2.)),
                                       {"example.thermal_pad", "1", "", ""},
                                       290.);
    require(passive.capabilities().noise, "explicit passive noise absent");
    const auto covariance = passive.noise_correlation(1e9).watts_per_hz;
    require(std::abs(covariance(0, 0).real() / (1.380649e-23 * 290. * .75) - 1.) < 1e-14,
            "thermal noise scaling changed");
    near(covariance(0, 1), 0.);
    TouchstoneData table;
    table.ports = 1;
    table.frequencies_hz = {1., 3.};
    table.matrices = {{1, {0.}}, {1, {.5}}};
    auto tabulated = adapt_device_model(
        TabulatedNoiseModel("table", table, {{1., 3.}, {{{1, {2e-20}}}, {{1, {4e-20}}}}}),
        {"example.table", "1", "", ""});
    require(tabulated.capabilities().linear && tabulated.capabilities().noise,
            "native noise provider not discovered");
    near(tabulated.s_parameters(2.)(0, 0), .25);
    require(std::abs(tabulated.noise_correlation(2.).watts_per_hz(0, 0).real() / 3e-20 - 1.) <
                1e-14,
            "tabulated noise interpolation changed");
    rejects<std::out_of_range>([&] {
        tabulated.noise_correlation(4.);
    });
    auto active = adapt_passive_model(StaticSParameterModel("active", {{0, "", 50.}}, {1, {2.}}),
                                      {"example.bad_passive", "1", "", ""},
                                      0.);
    rejects<std::invalid_argument>([&] {
        active.noise_correlation(1e9);
    });

    ModelFunctions mixed;
    mixed.scattering = [](double) {
        return SMatrix{2, {0., .25, .5, 0.}};
    };
    ModelAdapter complex_refs(
        {{"example.mixed", "1", "", ""}, "mixed", {{0, "a", {45., 5.}}, {1, "b", {80., -7.}}}},
        mixed);
    LinearNetwork independent;
    complex_refs.add_to(independent, 3e9);
    const auto matrix = independent.external_s({0, 1});
    near(matrix(1, 0), .5);
    near(independent.port_reference_impedance_ohms(1), {80., -7.});
}

void spectrum_and_polynomial() {
    const auto biased = adapt_polynomial_amplifier(MatchedPolynomialAmplifier("biased", {1., 2.}),
                                                   {"example.biased", "1", "", ""});
    require(!biased.capabilities().coherent && biased.capabilities().operating_point,
            "DC-biased model claims unsupported RF provenance");
    const auto adapter = adapt_polynomial_amplifier(
        MatchedPolynomialAmplifier("poly", {0., 2., .1}), {"example.poly", "1", "", ""});
    const auto caps = adapter.capabilities();
    require(caps.linear && caps.spectrum && caps.operating_point && caps.coherent && !caps.noise,
            "polynomial capability mismatch");
    const PowerWaveSpectrum input{1e8, {{2, .02}}};
    const auto result = analyze_spectrum(input, 50., {adapter.spectrum_stage("poly")});
    const auto &output = result.stages.front().output.amplitudes;
    near(output.at(2), .04);
    near(output.at(4), .1 * std::sqrt(50. / 2.) * .02 * .02);
    near(output.at(0), .1 * std::sqrt(50.) * .02 * .02);
    ModelOperatingPoint point{1e8, {{0, 2}, {1, 0}, {1, 2}, {1, 4}}, {.02, 0., 0., 0.}};
    const auto local = adapter.linearize(point);
    near(local.outgoing[1], output.at(0));
    near(local.outgoing[2], output.at(2));
    near(local.outgoing[3], output.at(4));
    // Independent finite difference checks the callable's complex Jacobian wiring.
    const Complex direction{.3, -.7};
    constexpr double step = 1e-6;
    auto plus = point, minus = point;
    plus.incident[0] += step * direction;
    minus.incident[0] -= step * direction;
    const auto high = adapter.linearize(plus), low = adapter.linearize(minus);
    for (std::size_t i = 0; i < point.channels.size(); ++i) {
        const auto derivative = local.jacobian.direct()(i, 0) * direction +
                                local.jacobian.conjugate()(i, 0) * std::conj(direction);
        require(std::abs((high.outgoing[i] - low.outgoing[i]) / (2. * step) - derivative) < 1e-10,
                "adapter Jacobian differs from finite difference");
    }
    auto mixer = adapt_device_model(IdealRealMixer("mixer", 5, 0.), {"example.mixer", "1", "", ""});
    require(mixer.capabilities().spectrum && !mixer.capabilities().linear, "mixer claims linear S");
    const auto mixed = mixer.transmit(input);
    near(mixed.amplitudes.at(3), .02);
    near(mixed.amplitudes.at(7), .02);
}

void custom_feedback() {
    ModelFunctions hooks;
    hooks.operating_point = [](const ModelOperatingPoint &p) {
        const auto a = p.incident.at(0);
        const auto power = std::norm(a);
        return ConversionLinearization{
            FrequencyConversionModel(
                p.spacing_hz, p.channels, {1, {1.5 - 4. * power}}, {1, {-2. * a * a}}),
            {(1.5 - 2. * power) * a}};
    };
    ModelAdapter adapter(one_port(), hooks);
    const Complex target{.1, .02}, outgoing = (1.5 - 2. * std::norm(target)) * target;
    FrequencyConversionModel empty(1e8, {{0, 1}}, {1, {0.}}, {1, {0.}});
    ConversionDevice device{
        empty, {target - .2 * outgoing}, {.2}, empty.zero_noise(), empty.zero_noise()};
    ConversionOperatingOptions options;
    options.relative_tolerance = 1e-12;
    options.absolute_tolerance = 1e-14;
    const auto result = solve_conversion_operating_point(
        {device}, {}, {adapter.operating_device(0, 1e8, {{0, 1}})}, {}, options);
    near(result.waves.incident[0], target);
    near(result.waves.outgoing[0], outgoing);
    require(result.iterations > 0, "custom model did not enter working-point engine");

    struct HostPhase {
        AmPmResponse response(double) const {
            return {.2, 0.};
        }
    };

    const auto base = adapter.linearize({1e8, {{0, 1}}, {target}});
    const auto rotated = apply_output_am_pm(base, HostPhase{}, {0});
    const auto phase = std::polar(1., .2);
    near(rotated.outgoing[0], phase * outgoing);
    near(rotated.jacobian.direct()(0, 0), phase * base.jacobian.direct()(0, 0));
    near(rotated.jacobian.conjugate()(0, 0), phase * base.jacobian.conjugate()(0, 0));
}

void malformed_contracts() {
    int calls = 0;
    ModelFunctions hooks;
    hooks.scattering = [&](double) {
        ++calls;
        return SMatrix{1, {0.}};
    };
    ModelAdapter valid(one_port(), hooks);
    rejects<std::invalid_argument>([&] {
        valid.s_parameters(-1.);
    });
    require(calls == 0, "invalid input reached model");
    hooks.scattering = [](double) {
        return SMatrix{2, {0., 0., 0., 0.}};
    };
    ModelAdapter bad_shape(one_port(), hooks);
    rejects<std::invalid_argument>([&] {
        bad_shape.s_parameters(1.);
    });
    hooks.scattering = [](double) {
        return SMatrix{1, {0.}};
    };
    hooks.noise = [](double) {
        return NoiseCorrelation{{1, {-1.}}};
    };
    ModelAdapter bad_noise(one_port(), hooks);
    rejects<std::invalid_argument>([&] {
        bad_noise.noise_correlation(1.);
    });
    hooks.operating_point = [&](const ModelOperatingPoint &p) {
        ++calls;
        return ConversionLinearization{
            FrequencyConversionModel(p.spacing_hz, {{0, 2}}, {1, {0.}}, {1, {0.}}), {0.}};
    };
    ModelAdapter bad_grid(one_port(), hooks);
    rejects<std::invalid_argument>([&] {
        bad_grid.linearize({1., {{1, 1}}, {0.}});
    });
    require(calls == 0, "bad physical port reached callback");
    rejects<std::invalid_argument>([&] {
        bad_grid.linearize({1., {{0, 1}}, {0.}});
    });
    require(calls == 1, "returned channel identity not checked");
    auto metadata = one_port();
    metadata.ports[0].id = 1;
    rejects<std::invalid_argument>([&] {
        ModelAdapter invalid(metadata, {});
    });
    metadata = one_port();
    metadata.ports[0].reference_impedance = {-1., 0.};
    rejects<std::invalid_argument>([&] {
        ModelAdapter invalid(metadata, {});
    });
}
} // namespace

int main() {
    try {
        linear_noise_and_ownership();
        spectrum_and_polynomial();
        custom_feedback();
        malformed_contracts();
        std::cout << "model adapter contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
