#include "rfmodel/linear_path.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    const std::vector<LinearPathStage> matched = {{"amplifier", {2, {0., 0., 10., 0.}}},
                                                  {"attenuator", {2, {0., 0.5, 0.5, 0.}}}};
    const auto budget = analyze_linear_path(matched, 0.001);
    near(budget.input_accepted_w, 0.001);
    near(budget.load_delivered_w, 0.025);
    near(*budget.stages[0].operating_gain, 100);
    near(*budget.stages[1].operating_gain, 0.25);
    near(*budget.stages[1].cumulative_operating_gain, 25);
    near(*budget.transducer_gain, 25);

    const std::vector<LinearPathStage> bilateral = {
        {"first", {2, {{0.2, 0.1}, {0.04, 0.01}, {2., 0.2}, {-0.1, 0.15}}}},
        {"second", {2, {{0.1, -0.2}, {0.3, 0.05}, {0.3, 0.05}, {0.2, 0.1}}}}};
    const Complex source{0.2, 0.1};
    const Complex load{-0.1, 0.2};
    const auto result = analyze_linear_path(bilateral, 0.002, source, load);
    // Compare the full-chain budget with independently extracted external S.
    LinearNetwork external;
    external.add(bilateral[0].scattering);
    external.add(bilateral[1].scattering);
    external.connect(1, 2);
    const auto s = external.external_s({0, 3});
    near(*result.transducer_gain, transducer_power_gain(s, source, load));
    near(*result.stages.back().cumulative_operating_gain, operating_power_gain(s, load));
    near(-result.stages[0].output.absorbed_w, result.stages[1].input.absorbed_w);
    near(*result.stages[0].operating_gain * *result.stages[1].operating_gain,
         *result.stages.back().cumulative_operating_gain);
    near(result.input_power_fraction * *result.stages.back().cumulative_operating_gain,
         *result.transducer_gain);
    const auto scaled = analyze_linear_path(bilateral, 0.02, source, load);
    near(scaled.load_delivered_w, 10 * result.load_delivered_w);
    near(*scaled.transducer_gain, *result.transducer_gain);

    // Source mismatch reduces available-power delivery even for a matched device.
    const auto source_mismatch = analyze_linear_path({matched.front()}, 1., 0.5);
    near(source_mismatch.input_power_fraction, 0.75);
    near(*source_mismatch.stages[0].operating_gain, 100);
    near(*source_mismatch.transducer_gain, 75);
    const auto reflective = analyze_linear_path({{"thru", {2, {0., 1., 1., 0.}}}}, 1., {}, 1.);
    near(reflective.load_delivered_w, 0.);
    require(!reflective.stages[0].operating_gain, "zero accepted power must not have a gain");
    const auto active_input = analyze_linear_path({{"active", {2, {2., 0., 1., 0.}}}}, 1.);
    require(active_input.input_accepted_w < 0, "signed net input power is preserved");
    require(!active_input.stages[0].operating_gain, "negative accepted power has no gain");
    near(*active_input.transducer_gain, 1.);

    rejects<std::invalid_argument>([] {
        analyze_linear_path({}, 1.);
    });
    rejects<std::invalid_argument>([&] {
        analyze_linear_path(matched, 0.);
    });
    rejects<std::invalid_argument>([&] {
        analyze_linear_path(matched, 1., 1.);
    });
    auto invalid = matched;
    invalid[1].reference_ohms = 75;
    rejects<std::invalid_argument>([&] {
        analyze_linear_path(invalid, 1.);
    });
    invalid = matched;
    invalid[1].name = invalid[0].name;
    rejects<std::invalid_argument>([&] {
        analyze_linear_path(invalid, 1.);
    });
}
