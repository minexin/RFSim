#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
Complex total(const StreamSnapshot &snapshot, int bin) {
    Complex result{};
    for (const auto &component : snapshot.reduction.components) {
        if (component.bin == bin) {
            result += component.amplitude;
        }
    }
    return result;
}

CoherentSystem::Plan owned_plan() {
    CoherentSystem system(1e8, 50., {{"rf", "clock"}});
    auto seed = system.input("in", {{2, SpectrumKind::source, 1., 1, .02}});
    auto adapter = adapt_polynomial_amplifier(MatchedPolynomialAmplifier("poly", {0., 2., .1}),
                                              {"example.poly", "1", "", ""});
    adapter.coherent_stage(system, "adapter", {seed}, {"out"});
    return system.compile();
}

void owning_adapter() {
    const auto plan = owned_plan();
    const auto result = plan.execute();
    near(total(result.stream("out"), 2), .04);
    near(total(result.stream("out"), 4), .1 * std::sqrt(25.) * .02 * .02);
    require(!result.stream("out").records.front().history.empty(), "provenance lost");
    require(result.stages().back().statistics.polynomial.has_value(), "statistics lost");
    near(total(plan.execute().stream("out"), 4), total(result.stream("out"), 4));
}

void custom_composition() {
    CoherentSystem system(1e8, 50., {{"rf", "clock"}});
    auto input = system.input("in", {{2, SpectrumKind::source, 1., 1, Complex{.02, .01}}});
    auto outputs = system.stage(
        "custom", {input}, {"twice", "six_times"}, [](auto &pipeline, const auto &inputs) {
            auto first = pipeline.polynomial(inputs.at(0), {0., 2.}, 11);
            auto second = pipeline.polynomial(first.output, {0., 3.}, 11);
            return CoherentSystem::StageEvaluation{{first.output, second.output}, {}};
        });
    const auto plan = system.compile();
    const auto result = plan.execute();
    near(total(result.stream(outputs[0]), 2), Complex{.04, .02});
    near(total(result.stream(outputs[1]), 2), Complex{.12, .06});
    require(result.usage().transformation_count == 2, "callback runtime operations miscounted");
    const auto &record = result.stream(outputs[1]).records.front();
    require(!record.history.empty(), "custom operation discarded history");
    require(result.history_root(record.history.front().factors.front().root_id).bin == 2,
            "custom operation changed root");
    auto frozen = system.compile();
    system.stage("alias", {outputs[1]}, {"alias_out"}, [](auto &, const auto &inputs) {
        return CoherentSystem::StageEvaluation{{inputs.at(0)}, {}};
    });
    require(frozen.stream_count() == 3, "plan not frozen");
    near(total(system.compile().execute().stream("alias_out"), 2), Complex{.12, .06});
}

void rejection_and_retry() {
    CoherentSystem system(1e8, 50., {{"rf", "clock"}});
    auto input = system.input("in", {{2, SpectrumKind::source, 1., 1, .02}});
    const auto before = system.compile();
    rejects<std::invalid_argument>([&] {
        system.stage("empty", {input}, {"out"}, {});
    });
    require(system.compile().stream_count() == before.stream_count(), "rejection mutated graph");
    bool fail = true;
    system.stage("retry", {input}, {"out"}, [&](auto &pipeline, const auto &inputs) {
        auto result = pipeline.polynomial(inputs.at(0), {0., 2.}, 11);
        if (fail) {
            throw std::runtime_error("injected after a pipeline operation");
        }
        return CoherentSystem::StageEvaluation{{result.output}, {}};
    });
    auto plan = system.compile();
    rejects<std::runtime_error>([&] {
        plan.execute();
    });
    fail = false;
    const auto result = plan.execute();
    near(total(result.stream("out"), 2), .04);
    require(result.usage().transformation_count == 1, "failed run contaminated retry");
    rejects<CoherentSystemCancelled>([&] {
        plan.execute([] {
            return true;
        });
    });
    near(total(result.stream("out"), 2), .04);

    system.stage("bad_count", {input}, {"bad"}, [](auto &, const auto &) {
        return CoherentSystem::StageEvaluation{};
    });
    rejects<std::invalid_argument>([&] {
        system.compile().execute();
    });
    CoherentPipeline foreign(1e8, 50., 1);
    auto foreign_stream = foreign.seed({{2, SpectrumKind::source, 1., 1, .02}});
    CoherentSystem other(1e8, 50., {{"rf", "clock"}});
    auto other_input = other.input("in", {{2, SpectrumKind::source, 1., 1, .02}});
    rejects<std::invalid_argument>([&] {
        other.stage("foreign_graph", {input}, {"bad"}, [](auto &, const auto &in) {
            return CoherentSystem::StageEvaluation{in, {}};
        });
    });
    other.stage("foreign_pipeline", {other_input}, {"bad"}, [foreign_stream](auto &, const auto &) {
        return CoherentSystem::StageEvaluation{{foreign_stream}, {}};
    });
    rejects<std::invalid_argument>([&] {
        other.compile().execute();
    });
}

void compression_and_reference() {
    auto adapter =
        adapt_saturating_amplifier("amp", SaturatingFundamentalCompression(10., 15., 18.));
    CoherentSystem system(1e8, 50., {{"rf", ""}});
    auto input = system.input("in", {{2, SpectrumKind::source, 1., 1, .02}});
    adapter.coherent_stage(system, "adapted", {input}, {"out"});
    auto direct = system.fundamental_compression(
        "direct", input, SaturatingFundamentalCompression(10., 15., 18.));
    const auto result = system.compile().execute();
    near(total(result.stream("out"), 2), total(result.stream(direct), 2));
    CoherentSystem wrong(1e8, 75., {{"rf", ""}});
    auto seed = wrong.input("in", {{2, SpectrumKind::source, 1., 1, .02}});
    adapter.coherent_stage(wrong, "adapted", {seed}, {"out"});
    rejects<std::invalid_argument>([&] {
        wrong.compile().execute();
    });
    rejects<std::invalid_argument>([&] {
        adapter.coherent_stage(system, "arity", {}, {"bad"});
    });
}
} // namespace

int main() {
    try {
        owning_adapter();
        custom_composition();
        rejection_and_retry();
        compression_and_reference();
        std::cout << "coherent stage extension passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
