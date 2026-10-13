#include "rfmodel/coherent_system.hpp"
#include "test_support.hpp"
#include <iostream>
#include <map>

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

void branched_physics() {
    CoherentSystem graph(1e8, 50., {{"rf", "clock"}, {"lo", "clock"}});
    const auto group = graph.source_group("rf");
    require(group == graph.source_group("lo"), "locked source assignment differs");
    const Complex wave{.02, .01};
    auto seed = graph.input("source", {{10, SpectrumKind::source, 1., group, wave}});
    std::vector<double> calls;
    auto ports = graph.linear_network(
        "split", {{seed, 0}}, {2, 0, 1}, {{"left", 1}, {"right", 2}}, [&](double frequency) {
            calls.push_back(frequency);
            LinearNetwork network;
            // Independently specified rows: the split produces i*a/2 and a/3.
            network.add({3, {0., 0., 0., Complex{0., .5}, 0., 0., 1. / 3., 0., 0.}});
            return network;
        });
    auto sum = graph
                   .linear_network("combine",
                                   {{ports[0], 0}, {ports[1], 1}},
                                   {0, 1, 2},
                                   {{"sum", 2}},
                                   [](double) {
                                       LinearNetwork network;
                                       network.add({3, {0., 0., 0., 0., 0., 0., 1., 2., 0.}});
                                       return network;
                                   })
                   .front();
    auto mixed = graph.mixer("if", sum, {12, group, 0., .4});
    auto squared = graph.polynomial("squared", mixed, {0., 0., .25}, 8);
    const auto plan = graph.compile();
    const auto first = plan.execute();
    require(calls == std::vector<double>{1e9}, "multi-output builder was not shared");
    const auto a = (Complex{0., .5} + 2. / 3.) * wave;
    near(total(first.stream(sum), 10), a);
    const auto low = std::conj(a) * std::polar(1., .4);
    const auto high = a * std::polar(1., .4);
    near(total(first.stream(mixed), 2), low);
    near(total(first.stream(mixed), 22), high);
    // Signed RMS voltage Fourier multiplication, not the product kernel.
    const auto scale = .25 * std::sqrt(50. / 2.);
    near(total(first.stream(squared), 4), scale * low * low);
    near(total(first.stream(squared), 44), scale * high * high);
    near(total(first.stream(squared), 24), 2. * scale * low * high);
    near(total(first.stream(squared), 20), 2. * scale * high * std::conj(low));
    require(first.stages().back().statistics.polynomial.has_value(), "missing stage statistics");
    // Compiled plan is an immutable snapshot despite later builder changes.
    graph.mixer("later", seed, {3, group, -6., 0.});
    const auto second = plan.execute();
    require(second.usage().stream_count == first.usage().stream_count, "plan grew with builder");
    near(total(second.stream("squared"), 24), total(first.stream(squared), 24));
    rejects<std::out_of_range>([&] {
        second.stream("later");
    });
    rejects<CoherentSystemCancelled>([&] {
        plan.execute([] {
            return true;
        });
    });
    // The existing snapshot remains readable after a cancelled run.
    near(total(first.stream(seed), 10), wave);
}

void cancellation_and_provenance() {
    CoherentSystem graph(1e8, 50., {{"source", ""}, {"lo", ""}});
    const auto g = graph.source_group("source");
    auto a = graph.input("a", {{5, SpectrumKind::source, 1., g, .02}});
    auto bank = graph.linear_network("fan", {{a, 0}}, {0, 1, 2}, {{"p", 1}, {"m", 2}}, [](double) {
        LinearNetwork n;
        n.add({3, {0., 0., 0., 1., 0., 0., -1., 0., 0.}});
        return n;
    });
    auto zero = graph
                    .linear_network("join",
                                    {{bank[0], 0}, {bank[1], 1}},
                                    {0, 1, 2},
                                    {{"cancelled_wave", 2}},
                                    [](double) {
                                        LinearNetwork n;
                                        n.add({3, {0., 0., 0., 0., 0., 0., 1., 1., 0.}});
                                        return n;
                                    })
                    .front();
    auto result = graph.compile().execute();
    near(total(result.stream(zero), 5), 0.);
    // Equal histories can sum to a zero coefficient, but the identity must survive.
    require(!result.stream(zero).records.empty(), "zero physical record was lost");
    require(!result.stream(zero).records.front().history.empty(), "zero source history was lost");
    auto before = graph.compile();
    rejects<std::invalid_argument>([&] {
        graph.input("a", {});
    });
    require(graph.compile().stream_count() == before.stream_count(),
            "duplicate modified definition");
    CoherentSystem other(1e8, 50., {{"source", ""}});
    auto foreign = other.input("source", {{5, SpectrumKind::source, 1., 1, .01}});
    rejects<std::invalid_argument>([&] {
        graph.mixer("bad", foreign, {2, 1, 0., 0.});
    });
    rejects<std::invalid_argument>([&] {
        result.stream(foreign);
    });
}

void mixer_bank_contract() {
    CoherentSystem empty(1e8, 50., {});
    rejects<std::invalid_argument>([&] {
        empty.input("bad", {{1, SpectrumKind::source, 1., 1, .1}});
    });
    auto empty_input = empty.input("empty", {});
    rejects<std::invalid_argument>([&] {
        empty.mixer("undeclared", empty_input, {2, 1, 0., 0.});
    });
    CoherentSystem graph(1e8, 50., {{"source", "clock"}, {"lo", "clock"}});
    auto seed = graph.input("seed", {{5, SpectrumKind::source, 1., 1, Complex{.02, .01}}});
    auto bank =
        graph.mixer_bank("bank", {{"a", seed, {7, 1, 0., .3}}, {"b", seed, {7, 1, 0., .3}}});
    const auto result = graph.compile().execute();
    near(total(result.stream(bank[0]), 2), std::conj(Complex{.02, .01}) * std::polar(1., .3));
    require(result.usage().transformation_count == 1, "bank charged multiple operations");
    require(result.stream(bank[0]).records.front().component.coherence_group ==
                result.stream(bank[1]).records.front().component.coherence_group,
            "bank lost shared coherence");
    CoherentPipeline pipeline(1e8, 50., 1);
    auto input = pipeline.seed({{5, SpectrumKind::source, 1., 1, .02}});
    const auto before = pipeline.usage();
    rejects<std::invalid_argument>([&] {
        pipeline.mixer_bank({{input, {7, 1, 0., 0.}}, {input, {0, 1, 0., 0.}}});
    });
    require(pipeline.usage().stream_count == before.stream_count &&
                pipeline.usage().transformation_count == before.transformation_count,
            "invalid bank published partial state");
}

void batch_atomicity() {
    CoherentPipeline pipeline(1e8, 50., 1);
    auto seed = pipeline.seed(
        {{1, SpectrumKind::source, 1., 1, .01}, {2, SpectrumKind::source, 1., 1, .02}});
    const auto usage = pipeline.usage();
    int calls = 0;
    rejects<std::runtime_error>([&] {
        pipeline.linear_network_outputs({{seed, 0}}, {0, 1, 2}, {1, 2}, [&](double) {
            if (++calls == 2) {
                throw std::runtime_error("host network error");
            }
            LinearNetwork n;
            n.add({3, {0., 0., 0., 1., 0., 0., .5, 0., 0.}});
            return n;
        });
    });
    require(pipeline.usage().stream_count == usage.stream_count &&
                pipeline.usage().transformation_count == usage.transformation_count,
            "failed multi-output batch published state");
    auto result = pipeline.linear_network_outputs({{seed, 0}}, {0, 1, 2}, {1, 2}, [](double) {
        LinearNetwork n;
        n.add({3, {0., 0., 0., 1., 0., 0., .5, 0., 0.}});
        return n;
    });
    require(result.size() == 2 && pipeline.usage().transformation_count == 1,
            "multi-output charged more than one operation");
    near(total(pipeline.inspect(result[1]), 2), .01);
}
} // namespace

int main() {
    try {
        branched_physics();
        cancellation_and_provenance();
        batch_atomicity();
        mixer_bank_contract();
        std::cout << "Typed coherent graph: shared multi-output networks, Fourier oracle, "
                     "snapshots and failure atomicity passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
