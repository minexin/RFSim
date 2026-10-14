#include <rfmodel/adapters/builtin_model_adapters.hpp>
#include <rfmodel/nonlinear/point_derivative_am_pm.hpp>
#include <rfmodel/am_pm.hpp>
#include <rfmodel/conversion/am_pm_linearization.hpp>

rfmodel::ModelAdapter make_installed_adapter();
double installed_vendor_phase();

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
    return 0;
}
