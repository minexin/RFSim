#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
TabulatedAmplifier table() {
    // Exact cubic A(r)=2r-.2r^3 and quadratic phi(r)=.3+.4r^2.
    return TabulatedAmplifier(
        {{0., 0., .3, 2., 0.}, {1., 1.8, .7, 1.4, .8}, {2., 2.4, 1.9, -.4, 1.6}});
}

Complex gain(double power) {
    return (2. - .2 * power) * std::polar(1., .3 + .4 * power);
}

Complex at(const StreamSnapshot &snapshot, int bin) {
    Complex result{};
    for (const auto &component : snapshot.reduction.components) {
        if (component.bin == bin) {
            result += component.amplitude;
        }
    }
    return result;
}

void linear_spectrum_and_jacobian() {
    const auto adapter = [] {
        return adapt_tabulated_amplifier("owned", table(), 75.);
    }();
    const auto caps = adapter.capabilities();
    require(caps.linear && !caps.noise && caps.spectrum && caps.operating_point && caps.coherent,
            "tabulated capabilities differ");
    require(adapter.descriptor().identity.vendor.empty(), "native table claims a vendor");
    near(adapter.references()[0], 75.);
    near(adapter.s_parameters(1e9)(1, 0), gain(0.));
    rejects<std::invalid_argument>([&] {
        adapter.s_parameters(0.);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.noise_correlation(1e9);
    });
    const auto sampled =
        adapter.conversion_device(1e8, {1, 3}, ModelNoisePolicy::omit_intrinsic_noise);
    near(sampled.model.direct()(1, 0), gain(0.));
    const Complex a{.3, .4}, b{-.2, .1};
    const double power = std::norm(a) + std::norm(b);
    const auto spectrum = adapter.transmit({1e8, {{0, 0.}, {1, a}, {3, b}}});
    require(spectrum.amplitudes.size() == 3, "table generated extra frequencies");
    near(spectrum.amplitudes.at(1), gain(power) * a);
    near(spectrum.amplitudes.at(3), gain(power) * b);
    near(spectrum.amplitudes.at(0), 0.);
    near(analyze_spectrum({1e8, {{1, a}, {3, b}}}, 75., {adapter.spectrum_stage("PA")})
             .stages[0]
             .output.amplitudes.at(1),
         gain(power) * a);
    const ModelOperatingPoint point{
        1e8, {{1, 3}, {0, 1}, {1, 1}, {0, 3}}, {{7., 2.}, a, {9., -4.}, b}};
    const auto local = adapter.linearize(point);
    near(local.outgoing[0], gain(power) * b);
    near(local.outgoing[2], gain(power) * a);
    near(local.outgoing[1], 0.);
    near(local.outgoing[3], 0.);
    const Complex gp = std::polar(1., .3 + .4 * power) * Complex{-.2, .4 * (2. - .2 * power)};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            Complex direct{}, conjugate{};
            if (point.channels[row].port == 1 && point.channels[col].port == 0) {
                const auto ar = point.channels[row].bin == 1 ? a : b;
                direct = ar * gp * std::conj(point.incident[col]);
                conjugate = ar * gp * point.incident[col];
                if (point.channels[row].bin == point.channels[col].bin) {
                    direct += gain(power);
                }
            }
            near(local.jacobian.direct()(row, col), direct);
            near(local.jacobian.conjugate()(row, col), conjugate);
        }
    }
}

CoherentSystem::Plan owned_plan() {
    CoherentSystem graph(1e8, 50., {{"rf", "clock"}});
    auto source = graph.input("in", {{1, SpectrumKind::source, 1., 1, Complex{.1, .2}}});
    auto adapter = adapt_tabulated_amplifier("PA", table());
    auto out = adapter.coherent_stage(graph, "PA", {source}, {"amplified"}).front();
    graph.polynomial("harmonics", out, {0., 0., .1}, 11);
    return graph.compile();
}

void coherent_power_phase_and_history() {
    const auto plan = owned_plan();
    const auto result = plan.execute();
    const Complex input{.1, .2}, output = gain(std::norm(input)) * input;
    near(at(result.stream("amplified"), 1), output);
    near(at(result.stream("harmonics"), 2), .5 * output * output);
    const auto &stats = result.stages().at(1).statistics;
    require(stats.common_complex_gain.has_value(), "complex gain statistic absent");
    near(*stats.common_complex_gain, gain(std::norm(input)));
    near(*stats.input_power_w, std::norm(input));
    require(result.stream("harmonics").records.front().history.front().factors.size() == 2,
            "table stage broke recursive history");
    near(at(plan.execute().stream("harmonics"), 2), .5 * output * output);

    // Coherent cancellation determines the table radius, even when each path
    // exceeds the table domain. A nonzero small-signal gain retains zero history.
    CoherentSystem cancelled(1e8, 50., {{"rf", "clock"}});
    const auto in = cancelled.input(
        "cancelled", {{1, SpectrumKind::source, 1., 1, 3.}, {1, SpectrumKind::source, 1., 1, -3.}});
    adapt_tabulated_amplifier("cancel PA", table()).coherent_stage(cancelled, "PA", {in}, {"out"});
    const auto zero = cancelled.compile().execute();
    near(zero.stream("out").reduction.total_power_w, 0.);
    near(*zero.stages().back().statistics.common_complex_gain, gain(0.));
    require(!zero.stream("out").records.empty() &&
                !zero.stream("out").records.front().history.empty(),
            "cancelled provenance lost");

    CoherentSystem independent(1e8, 50., {{"a", ""}, {"b", ""}});
    const auto sources = independent.input(
        "in", {{1, SpectrumKind::source, 1., 1, .3}, {1, SpectrumKind::source, 1., 2, -.4}});
    adapt_tabulated_amplifier("PA", table()).coherent_stage(independent, "PA", {sources}, {"out"});
    const auto separate = independent.compile().execute();
    near(*separate.stages().back().statistics.input_power_w, .25);
    near(separate.stream("out").reduction.total_power_w, .25 * std::norm(gain(.25)));
    require(separate.stream("out").reduction.components.size() == 2, "independent groups merged");

    CoherentSystem matched(1e8, 75., {{"rf", "clock"}});
    auto matched_input = matched.input("in", {{1, SpectrumKind::source, 1., 1, input}});
    adapt_tabulated_amplifier("75-ohm table", table(), 75.)
        .coherent_stage(matched, "table", {matched_input}, {"out"});
    near(at(matched.compile().execute().stream("out"), 1), output);
}

void distortion_and_failure() {
    const Complex a{.1, .2}, harmonic = .5 * a * a;
    auto graph = [](AmplifierInputMode mode) {
        CoherentSystem system(1e8, 50., {{"rf", "clock"}});
        auto source = system.input("in", {{1, SpectrumKind::source, 1., 1, Complex{.1, .2}}});
        auto nonlinear = system.polynomial("first", source, {0., 1., .1}, 11);
        adapt_tabulated_amplifier("table", table(), 50., mode)
            .coherent_stage(system, "table", {nonlinear}, {"out"});
        return system.compile();
    };
    rejects<std::invalid_argument>([&] {
        graph(AmplifierInputMode::sources_only).execute();
    });
    const auto result = graph(AmplifierInputMode::propagate_distortion).execute();
    const double power = std::norm(a) + std::norm(harmonic);
    near(at(result.stream("out"), 1), gain(power) * a);
    near(at(result.stream("out"), 2), gain(power) * harmonic);
    for (const auto &record : result.stream("out").records) {
        if (record.component.bin == 2) {
            require(record.history.front().factors.size() == 2, "propagated harmonic root lost");
        }
    }
    const auto model = adapt_tabulated_amplifier("table", table());
    rejects<std::out_of_range>([&] {
        model.transmit({1e8, {{1, 2.1}}});
    });
    rejects<std::invalid_argument>([&] {
        model.transmit({1e8, {{0, .1}}});
    });
    rejects<std::invalid_argument>([&] {
        adapt_tabulated_amplifier("bad", table(), 50., static_cast<AmplifierInputMode>(100));
    });
    rejects<std::invalid_argument>([&] {
        adapt_tabulated_amplifier("bad", table(), -1.);
    });
    CoherentSystem wrong_reference(1e8, 75., {{"rf", ""}});
    auto input = wrong_reference.input("in", {{1, SpectrumKind::source, 1., 1, .1}});
    model.coherent_stage(wrong_reference, "table", {input}, {"out"});
    rejects<std::invalid_argument>([&] {
        wrong_reference.compile().execute();
    });
    CoherentSystem outside(1e8, 50., {{"rf", ""}});
    auto large = outside.input("in", {{1, SpectrumKind::source, 1., 1, 2.1}});
    model.coherent_stage(outside, "table", {large}, {"out"});
    const auto failed = outside.compile();
    for (int i = 0; i < 2; ++i) {
        rejects<std::out_of_range>([&] {
            failed.execute();
        });
    }
    near(at(result.stream("out"), 1), gain(power) * a); // Old snapshot remains valid.
}
} // namespace

int main() {
    try {
        linear_spectrum_and_jacobian();
        coherent_power_phase_and_history();
        distortion_and_failure();
        std::cout << "tabulated model adapter passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
