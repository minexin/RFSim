#include <rfmodel/adapters/builtin_model_adapters.hpp>
#include <rfmodel/nonlinear/point_derivative_am_pm.hpp>
#include <rfmodel/am_pm.hpp>
#include <rfmodel/conversion/am_pm_linearization.hpp>
#include <rfmodel/models/ideal_devices.hpp>

rfmodel::ModelAdapter make_installed_adapter();
double installed_vendor_phase();
rfmodel::ConversionNonlinearDevice make_installed_bilinear_device();
rfmodel::ModelAdapter make_installed_highorder_adapter();

int main() {
    using namespace rfmodel;
    auto adapter = make_installed_adapter();
    const auto stage = adapter.spectrum_stage("owned");
    const auto spectrum = analyze_spectrum({1e8, {{2, .02}}}, 50., {stage});
    if (std::abs(spectrum.stages[0].output.amplitudes.at(2) - Complex{.04, 0.}) > 1e-14) {
        return 1;
    }
    CoherentSystem system(1e8, 50., {{"rf", "clock"}});
    auto seed = system.input("rf", {{2, SpectrumKind::source, 1., 1, .02}});
    adapter.coherent_stage(system, "amp", {seed}, {"out"});
    const auto result = system.compile().execute();
    if (std::abs(result.stream("out").reduction.total_power_w - .0016) > 1e-14) {
        return 2;
    }
    const auto local = adapter.linearize({1e8, {{0, 2}, {1, 2}}, {.02, 0.}});
    if (std::abs(local.outgoing[1] - Complex{.04, 0.}) > 1e-14) {
        return 3;
    }
    const SystemVue2023PointDerivativeAmPm vendor(-5., 10.);
    if (vendor.response(.01).phase_rad != installed_vendor_phase()) {
        return 4;
    }
    const auto phased = apply_output_am_pm(local, vendor, {1});
    if (std::abs(phased.outgoing[1] - vendor.transmit(.04)) > 1e-14) {
        return 5;
    }
    const auto pad = adapt_passive_model(
        MatchedTransmissionModel("pad", 20. * std::log10(2.)), {"consumer.pad", "1", "", ""}, 290.);
    auto converted = pad.conversion_device(1e8, {8, 12}, ModelNoisePolicy::require_model_noise);
    converted.source[2] = 1.;
    FrequencyConversionNetwork network(1e8);
    network.add(converted);
    network.add(ideal_mixer_conversion(1e8, {{0, 8}, {0, 12}, {1, 2}, {1, 18}, {1, 22}}, 10, 0.));
    network.connect(0, 1, 1, 0);
    const auto mixed = network.analyze();
    if (std::abs(mixed.outgoing[6] - Complex{.5, 0.}) > 1e-14 ||
        std::abs(mixed.outgoing_noise.covariance(6, 6).real() / (1.380649e-23 * 290.) - 1.5) >
            1e-12) {
        return 6;
    }
    const auto highorder = make_installed_highorder_adapter();
    const auto caps = highorder.capabilities();
    if (caps.linear || caps.noise || caps.spectrum || !caps.operating_point || !caps.coherent) {
        return 7;
    }
    const auto high_local = highorder.linearize({1e8, {{0, 1}, {1, 1}, {1, 2}}, {.01, 0., 0.}});
    const double harmonic = .1 * std::sqrt(75. / 2.) * .01 * .01;
    if (std::abs(high_local.outgoing[2] - harmonic) > 1e-14) {
        return 8;
    }
    const auto plan = [] {
        CoherentSystem graph(1e8, 75., {{"rf", "clock"}});
        auto source = graph.input("in", {{1, SpectrumKind::source, 1., 1, .01}});
        make_installed_highorder_adapter().coherent_stage(graph, "amp", {source}, {"out"});
        return graph.compile();
    }();
    for (int i = 0; i < 2; ++i) {
        Complex measured{};
        const auto result = plan.execute();
        for (const auto &component : result.stream("out").reduction.components) {
            if (component.bin == 2) {
                measured += component.amplitude;
            }
        }
        if (std::abs(measured - harmonic) > 1e-14) {
            return 9;
        }
    }
    // The callback crosses a translation unit and outlives both owning adapters.
    const auto mixer_device = make_installed_bilinear_device();
    const Complex rf{.2, -.3}, lo{.8, .4};
    const auto mixer = mixer_device.evaluate({.7, lo, -.6, rf});
    if (mixer_device.device != 3 || std::abs(mixer.outgoing[0] - .5 * rf * std::conj(lo)) > 1e-14 ||
        std::abs(mixer.outgoing[2] - .5 * rf * lo) > 1e-14 || mixer.outgoing[1] != Complex{} ||
        mixer.outgoing[3] != Complex{} ||
        mixer.jacobian.reference_impedances_ohms() != std::vector<Complex>(4, 75.)) {
        return 10;
    }
    const auto mixer_here = adapt_bilinear_mixer("second translation unit", 0., 2., 75.);
    const auto mixer_local =
        mixer_here.linearize({1e6, {{2, 2}, {1, 10}, {2, 22}, {0, 12}}, {.7, lo, -.6, rf}});
    if (mixer_local.outgoing != mixer.outgoing) {
        return 11;
    }
    return 0;
}
