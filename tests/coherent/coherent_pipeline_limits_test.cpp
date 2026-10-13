#include "rfmodel/coherent/coherent_pipeline.hpp"
#include "../support/test_support.hpp"
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

using namespace rfmodel;

namespace {
CoherentComponent source(int bin = 8, std::uint64_t group = 1, Complex amplitude = {.01, 0.}) {
    return {bin, SpectrumKind::source, 1., group, amplitude};
}

LinearNetwork transfer_network(double, double transfer) {
    LinearNetwork network(50.);
    network.add({2, {0., 0., transfer, 0.}});
    return network;
}

LinearNetwork through(double f) {
    return transfer_network(f, 1.);
}

bool same_usage(const PipelineUsage &a, const PipelineUsage &b) {
    return std::tie(a.stream_count,
                    a.transformation_count,
                    a.physical_record_count,
                    a.history_term_count,
                    a.history_factor_count,
                    a.physical_root_count,
                    a.history_root_count,
                    a.physical_identity_factor_count) == std::tie(b.stream_count,
                                                                  b.transformation_count,
                                                                  b.physical_record_count,
                                                                  b.history_term_count,
                                                                  b.history_factor_count,
                                                                  b.physical_root_count,
                                                                  b.history_root_count,
                                                                  b.physical_identity_factor_count);
}

void failures() {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    rejects<std::invalid_argument>([&] {
        CoherentPipeline p(nan, 50., 2);
    });
    rejects<std::invalid_argument>([&] {
        CoherentPipeline p(1e8, nan, 2);
    });
    rejects<std::invalid_argument>([] {
        CoherentPipeline p(1e8, 0., 2);
    });
    rejects<std::invalid_argument>([] {
        CoherentPipeline p(1e8, 50., 0);
    });
    CoherentPipeline p(1e8, 50., 2), other(1e8, 50., 2);
    const auto empty = p.seed({});
    const auto input = p.seed({source()});
    const auto before = p.usage();
    const auto *address = &p.inspect(input);
    rejects<std::invalid_argument>([&] {
        p.inspect(other.seed({}));
    });
    rejects<std::invalid_argument>([&] {
        p.mix(input, {8, 2, 0., 0.});
    });
    rejects<std::invalid_argument>([&] {
        p.mix(empty, {0, 2, 0., 0.});
    });
    rejects<std::invalid_argument>([&] {
        p.mix(empty, {10, 0, 0., 0.});
    });
    rejects<std::invalid_argument>([&] {
        p.mix(empty, {10, 3, 0., 0.});
    });
    rejects<std::invalid_argument>([&] {
        p.mix(empty, {10, 2, nan, 0.});
    });
    rejects<std::invalid_argument>([&] {
        p.mix(empty, {10, 2, 0., nan});
    });
    rejects<std::overflow_error>([&] {
        p.mix(empty, {10, 2, 1e9, 0.});
    });
    rejects<std::invalid_argument>([&] {
        p.polynomial(empty, {}, 1);
    });
    rejects<std::invalid_argument>([&] {
        p.polynomial(empty, {1., 1.}, 1);
    });
    rejects<std::invalid_argument>([&] {
        p.polynomial(empty, {0., nan}, 1);
    });
    rejects<std::invalid_argument>([&] {
        p.polynomial(empty, {0., 1.}, 0);
    });
    rejects<std::invalid_argument>([&] {
        p.polynomial(empty, {0., 1.}, 257);
    });
    rejects<std::invalid_argument>([&] {
        p.seed({source(8, 3)});
    });
    auto harmonic = source();
    harmonic.kind = SpectrumKind::harmonic;
    rejects<std::invalid_argument>([&] {
        p.seed({harmonic});
    });
    rejects<std::invalid_argument>([&] {
        p.linear_network({{empty, 3}}, {0, 1}, 1, through);
    });
    rejects<std::invalid_argument>([&] {
        p.linear_network({}, {0, 0}, 0, through);
    });
    rejects<std::invalid_argument>([&] {
        p.linear_network({}, {0, 1}, 2, through);
    });
    rejects<std::invalid_argument>([&] {
        p.linear_network({}, {0, 1}, 1, {});
    });
    rejects<std::invalid_argument>([&] {
        p.linear_network({{empty, 0}}, {0, 1}, 1, [](double) {
            LinearNetwork network(75.);
            network.add({2, {0., 0., 0., 0.}}, 75.);
            return network;
        });
    });
    rejects<std::runtime_error>([&] {
        p.linear_network({{input, 0}}, {0, 1}, 1, [](double) -> LinearNetwork {
            throw std::runtime_error("deliberate builder failure");
        });
    });
    rejects<std::logic_error>([&] {
        p.linear_network({{input, 0}}, {0, 1}, 1, [&](double f) {
            p.seed({source(12)});
            return through(f);
        });
    });
    require(same_usage(before, p.usage()), "failed operations changed committed counters");
    require(&p.inspect(input) == address, "failed operations moved snapshot");
    require(p.inspect(input).records[0].component.amplitude == Complex(.01, 0.),
            "failed operations changed wave");
    rejects<std::out_of_range>([&] {
        p.history_root(2);
    });
    rejects<std::out_of_range>([&] {
        p.physical_root(2);
    });
    const auto mixed = p.mix(input, {10, 2, 0., 0.});
    require(p.history_root(2).role == RootRole::lo, "failure consumed history registry ID");
    require(p.physical_root(2).role == RootRole::mixed, "failure consumed physical registry ID");
    require(p.inspect(mixed).records[0].component.coherence_group == 3,
            "failure consumed group ID");
    require(&p.inspect(input) == address, "successful operation moved snapshot");
    const auto poly = p.polynomial(input, {0., 0., 1.}, 2);
    rejects<std::invalid_argument>([&] {
        p.compress_fundamentals(poly.output, SaturatingFundamentalCompression(20., 20., 23.));
    });
    CoherentPipeline exhausted(1e8, 50., std::numeric_limits<std::uint64_t>::max());
    const auto maxgroup = exhausted.seed({source()});
    rejects<std::overflow_error>([&] {
        exhausted.polynomial(maxgroup, {0., 1.}, 1);
    });
    rejects<std::overflow_error>([&] {
        exhausted.mix(maxgroup, {10, 2, 0., 0.});
    });
    CoherentPipeline frequency(1., 50., 2);
    const auto high = frequency.seed({source(std::numeric_limits<int>::max())});
    rejects<std::overflow_error>([&] {
        frequency.mix(high, {2, 2, 0., 0.});
    });
    rejects<std::overflow_error>([&] {
        frequency.polynomial(high, {0., 0., 1.}, 1);
    });
}

void stale_handle() {
    alignas(CoherentPipeline) unsigned char storage[sizeof(CoherentPipeline)];
    auto *first = new (storage) CoherentPipeline(1e8, 50., 2);
    const auto stale = first->seed({source()});
    first->~CoherentPipeline();
    auto *second = new (storage) CoherentPipeline(1e8, 50., 2);
    second->seed({source()});
    rejects<std::invalid_argument>([&] {
        second->inspect(stale);
    });
    second->~CoherentPipeline();
}

void expanded_boundaries() {
    CoherentPipeline p(1e8, 50., 5000);
    std::vector<CoherentComponent> sources;
    for (std::uint64_t group = 1; group <= 4096; ++group) {
        sources.push_back(source(8, group, {.0001, 0.}));
    }
    const auto large = p.seed(sources);
    const auto linear = p.polynomial(large, {0., 1.}, 1);
    require(linear.counts.expanded_input_count == 4096, "4096 expanded inputs not accepted");
    const auto extra = p.seed({source(8, 4097)});
    const auto before = p.usage();
    rejects<std::length_error>([&] {
        p.linear_network({{large, 0}, {extra, 0}}, {0, 1}, 1, through);
    });
    rejects<std::length_error>([&] {
        p.polynomial(large, {0., 1., 1.}, 1);
    });
    require(same_usage(before, p.usage()), "expanded failure changed pipeline");
    sources.push_back(source(8, 4097));
    rejects<std::invalid_argument>([&] {
        p.seed(sources);
    });

    CoherentPipeline histories(1e8, 50., 2);
    std::vector<CoherentPipeline::PortStream> branches;
    for (int i = 0; i < 65; ++i) {
        const auto branch = histories.mix(histories.seed({source(100 + i)}), {90 + i, 2, 0., 0.});
        branches.push_back({branch, 0});
    }
    const auto combined = histories.linear_network(branches, {0, 1}, 1, [](double f) {
        return transfer_network(f, f == 10e8 ? 1. : 0.);
    });
    require(histories.inspect(combined).records.front().history.size() == 65,
            "65 histories failed to coalesce physically");
    const auto counts = histories.usage();
    rejects<std::length_error>([&] {
        histories.polynomial(combined, {0., 1., 1.}, 1);
    });
    require(same_usage(counts, histories.usage()), "low cutoff hid native input failure");
}

void native_stage_limits() {
    CoherentPipeline polynomial(1e8, 50., 20);
    std::vector<CoherentComponent> tones;
    for (int i = 0; i < 16; ++i) {
        tones.push_back(source(100 + i, static_cast<std::uint64_t>(i + 1)));
    }
    const auto input = polynomial.seed(tones);
    const auto before = polynomial.usage();
    bool native_path_cap = false;
    try {
        polynomial.polynomial(input, {0., 0., 0., 0., 1.}, 1);
    } catch (const std::length_error &error) {
        native_path_cap = std::string(error.what()).find("4096 RF terms") != std::string::npos;
    }
    require(native_path_cap && same_usage(before, polynomial.usage()),
            "low cutoff hid native generated-path limit or mutated state");

    CoherentPipeline mixer(1e8, 50., 5000);
    tones.clear();
    for (std::uint64_t group = 1; group <= 2048; ++group) {
        tones.push_back(source(20, group));
    }
    const auto allowed = mixer.mix(mixer.seed(tones), {1, 5000, 0., 0.});
    require(mixer.inspect(allowed).records.size() == 4096, "2048 mixer inputs not accepted");
    tones.push_back(source(20, 2049));
    const auto excess = mixer.seed(tones);
    const auto mixer_before = mixer.usage();
    rejects<std::invalid_argument>([&] {
        mixer.mix(excess, {1, 5000, 0., 0.});
    });
    require(same_usage(mixer_before, mixer.usage()), "2049 mixer rejection changed state");

    CoherentPipeline network(1e8, 50., 1);
    tones.clear();
    for (int bin = 1; bin <= 2049; ++bin) {
        tones.push_back(source(bin));
    }
    const auto excess_bins = network.seed(tones);
    const auto network_before = network.usage();
    std::size_t calls = 0;
    rejects<std::length_error>([&] {
        network.linear_network({{excess_bins, 0}}, {0, 1}, 1, [&](double f) {
            ++calls;
            return through(f);
        });
    });
    require(calls == 0 && same_usage(network_before, network.usage()),
            "2049-frequency failure called builder or mutated state");
    tones.pop_back();
    const auto allowed_bins = network.seed(tones);
    const auto passed = network.linear_network({{allowed_bins, 0}}, {0, 1}, 1, [&](double f) {
        ++calls;
        return through(f);
    });
    require(calls == 2048 && network.inspect(passed).records.size() == 2048,
            "2048-frequency network boundary not accepted");
}

void factor_and_storage_limits() {
    CoherentPipeline p(1e8, 50., 2);
    auto stream = p.seed({source(8, 1, {.2, 0.})});
    for (int i = 0; i < 8; ++i) {
        stream = p.polynomial(stream, {0., 0., 1.}, 256).output;
    }
    require(p.inspect(stream).records[0].history[0].factors.size() == 256,
            "256 source factors not retained");
    const auto before = p.usage();
    rejects<std::length_error>([&] {
        p.mix(stream, {3, 2, 0., 0.});
    });
    require(same_usage(before, p.usage()), "factor failure changed registry/counters");
    const auto dropped = p.polynomial(stream, {0., 0., 1.}, 256);
    require(dropped.counts.generated_term_count == 1 && dropped.counts.discarded_term_count == 1 &&
                dropped.counts.discarded_by_source_order.at(512) == 1,
            "source cutoff did not precede 256-factor composition guard");
    require(p.inspect(dropped.output).records.empty(), "over-order term survived");

    CoherentPipeline records(1e8, 50., 4096);
    std::vector<CoherentComponent> many;
    for (std::uint64_t group = 1; group <= 4096; ++group) {
        many.push_back(source(8, group));
    }
    for (int i = 0; i < 16; ++i) {
        records.seed(many);
    }
    require(records.usage().physical_record_count == 65536 &&
                records.usage().history_term_count == 65536,
            "cumulative storage boundary incorrect");
    const auto full = records.usage();
    rejects<std::length_error>([&] {
        records.seed({source()});
    });
    require(same_usage(full, records.usage()), "storage failure changed cumulative usage");

    // Reach the cumulative factor cap independently of the record/term caps.
    CoherentPipeline factors(1e8, 50., 16);
    std::vector<CoherentPipeline::PortStream> factor_inputs;
    for (std::uint64_t group = 1; group <= 16; ++group) {
        auto high_order = factors.seed({source(8, group, {.2, 0.})});
        for (int i = 0; i < 8; ++i) {
            high_order = factors.polynomial(high_order, {0., 0., 1.}, 256).output;
        }
        factor_inputs.push_back({high_order, 0});
    }
    const auto factor_stream = factors.linear_network(factor_inputs, {0, 1}, 1, through);
    while (1048576 - factors.usage().history_factor_count >= 4096) {
        factors.linear_network({{factor_stream, 0}}, {0, 1}, 1, through);
    }
    require(factors.usage().history_factor_count == 1048560,
            "cumulative factor boundary differs from hand-counted 16*(1+2+...+256)");
    const auto full_factors = factors.usage();
    rejects<std::length_error>([&] {
        factors.linear_network({{factor_stream, 0}}, {0, 1}, 1, through);
    });
    require(same_usage(full_factors, factors.usage()), "factor storage failure changed usage");

    CoherentPipeline operations(1e8, 50., 2);
    const auto empty = operations.seed({});
    for (int i = 0; i < 512; ++i) {
        operations.polynomial(empty, {0., 1.}, 1);
    }
    rejects<std::length_error>([&] {
        operations.polynomial(empty, {0., 1.}, 1);
    });
    operations.seed({});
    require(operations.usage().transformation_count == 512, "seed consumed transformation budget");
    CoherentPipeline streams(1e8, 50., 2);
    for (int i = 0; i < 4096; ++i) {
        streams.seed({});
    }
    rejects<std::length_error>([&] {
        streams.seed({});
    });
}
} // namespace

int main() {
    failures();
    stale_handle();
    expanded_boundaries();
    native_stage_limits();
    factor_and_storage_limits();
}
