#include "rfmodel/coherent_pipeline.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <tuple>
#include <vector>

// Exercise every allocation boundary, including the last allocation before the
// stream commit. This executable is single-threaded and does not replace any
// production allocator or expose pipeline internals.
namespace allocation_failure {
std::ptrdiff_t remaining = -1;
std::ptrdiff_t allowance = -1;
const char *phase = "startup";
} // namespace allocation_failure

// Keep replacement allocation/deallocation calls opaque to optimizers. In
// particular, inlining a malloc-backed replacement delete makes GCC diagnose
// legitimate matched new/delete pairs as mismatched malloc/free operations.
#if defined(__GNUC__) || defined(__clang__)
#define RFMODEL_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define RFMODEL_TEST_NOINLINE __declspec(noinline)
#else
#define RFMODEL_TEST_NOINLINE
#endif
RFMODEL_TEST_NOINLINE void *operator new(std::size_t size) {
    if (allocation_failure::remaining == 0) {
        throw std::bad_alloc();
    }
    if (allocation_failure::remaining > 0) {
        --allocation_failure::remaining;
    }
    if (void *memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

RFMODEL_TEST_NOINLINE void *operator new[](std::size_t size) {
    return ::operator new(size);
}

RFMODEL_TEST_NOINLINE void operator delete(void *memory) noexcept {
    std::free(memory);
}

RFMODEL_TEST_NOINLINE void operator delete[](void *memory) noexcept {
    std::free(memory);
}

RFMODEL_TEST_NOINLINE void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

RFMODEL_TEST_NOINLINE void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

#undef RFMODEL_TEST_NOINLINE

using namespace rfmodel;

namespace {
constexpr auto propagated = AmplifierInputMode::propagate_distortion;

CoherentComponent source(int bin = 100, std::uint64_t group = 1, Complex amplitude = {.01, 0.}) {
    return {bin, SpectrumKind::source, 1., group, amplitude};
}

CoherentLimitedAmplifier limited(double reference = 50.) {
    return {0., 100., 103., 50., 50., reference};
}

CoherentHighOrderAmplifier highorder(const std::vector<double> &coefficients = {},
                                     double reference = 50.) {
    return {0., 100., 103., coefficients, reference};
}

LinearNetwork transfer(double gain = 1.) {
    LinearNetwork network(50.);
    network.add({2, {0., 0., gain, 0.}});
    return network;
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

bool same_component(const CoherentComponent &a, const CoherentComponent &b) {
    return std::tie(a.bin, a.kind, a.bandwidth_hz, a.coherence_group, a.amplitude) ==
           std::tie(b.bin, b.kind, b.bandwidth_hz, b.coherence_group, b.amplitude);
}

void same_snapshot(const StreamSnapshot &a, const StreamSnapshot &b) {
    require(a.records.size() == b.records.size(), "snapshot record count changed");
    for (std::size_t i = 0; i < a.records.size(); ++i) {
        const auto &left = a.records[i];
        const auto &right = b.records[i];
        require(same_component(left.component, right.component), "snapshot component changed");
        require(left.physical_identity.factors == right.physical_identity.factors,
                "snapshot physical identity changed");
        require(left.history.size() == right.history.size(), "snapshot history count changed");
        for (std::size_t j = 0; j < left.history.size(); ++j) {
            require(left.history[j].factors == right.history[j].factors &&
                        left.history[j].amplitude == right.history[j].amplitude,
                    "snapshot history changed");
        }
    }
    require(a.reduction.total_power_w == b.reduction.total_power_w &&
                a.reduction.power_by_bin_w == b.reduction.power_by_bin_w &&
                a.reduction.components.size() == b.reduction.components.size(),
            "snapshot reduction changed");
    for (std::size_t i = 0; i < a.reduction.components.size(); ++i) {
        require(same_component(a.reduction.components[i], b.reduction.components[i]),
                "reduced component changed");
    }
}

template <class Exception, class Function>
void unchanged_failure(CoherentPipeline &pipeline,
                       CoherentPipeline::Stream input,
                       Function action) {
    const auto usage = pipeline.usage();
    const auto *address = &pipeline.inspect(input);
    const auto saved = *address;
    rejects<Exception>(action);
    require(same_usage(usage, pipeline.usage()), "failed adapter changed committed usage");
    require(address == &pipeline.inspect(input), "failed adapter moved an old snapshot");
    same_snapshot(saved, pipeline.inspect(input));
}

void empty_silent_and_validation() {
    CoherentPipeline pipeline(1e8, 50., 2), other(1e8, 50., 2);
    const auto empty = pipeline.seed({});
    const auto silent = pipeline.seed({source(100, 1, {})});
    const auto foreign = other.seed({});
    const auto before = pipeline.usage();
    const auto a = pipeline.limited_amplifier(empty, limited());
    const auto b = pipeline.highorder_amplifier(empty, highorder(), 1);
    require(pipeline.inspect(a.output).records.empty() &&
                pipeline.inspect(b.output).records.empty(),
            "empty amplifier created records");
    require(a.input_power_w == 0. && b.input_power_w == 0. &&
                a.operating_point.fundamental_amplitude_gain == 1. &&
                b.operating_point.nonlinear_input_scale == 1. && a.direct_contribution_count == 0 &&
                b.nonlinear_counts.expanded_input_count == 0,
            "empty amplifier counts or zero-drive operating point incorrect");
    require(pipeline.usage().stream_count == before.stream_count + 2 &&
                pipeline.usage().transformation_count == before.transformation_count + 2,
            "adapter must commit exactly one stream and transformation");
    for (bool use_limited : {true, false}) {
        const auto result = use_limited
                                ? pipeline.limited_amplifier(silent, limited())
                                : pipeline.highorder_amplifier(silent, highorder({1., 1.}), 3);
        const auto &record = pipeline.inspect(result.output).records.at(0);
        require(record.history.size() == 1 && record.history[0].amplitude == Complex{} &&
                    record.component.amplitude == Complex{},
                "silent known history was pruned");
        require(result.direct_contribution_count == 1 &&
                    result.discarded_direct_contributions == 0 &&
                    result.nonlinear_counts.physical_input_count == 1 &&
                    result.nonlinear_counts.expanded_input_count == 1 &&
                    result.nonlinear_counts.generated_term_count == 0,
                "silent histories must count as expanded inputs, not active products");
    }
    const auto invalid_mode = static_cast<AmplifierInputMode>(12345);
    unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
        pipeline.limited_amplifier(empty, limited(), invalid_mode);
    });
    unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
        pipeline.highorder_amplifier(empty, highorder(), 1, invalid_mode);
    });
    unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
        pipeline.limited_amplifier(empty, limited(75.));
    });
    unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
        pipeline.highorder_amplifier(empty, highorder({}, 75.), 1);
    });
    for (std::size_t cutoff : {std::size_t{0}, std::size_t{257}}) {
        unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
            pipeline.highorder_amplifier(empty, highorder(), cutoff);
        });
    }
    unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
        pipeline.limited_amplifier(foreign, limited());
    });
    unchanged_failure<std::invalid_argument>(pipeline, empty, [&] {
        pipeline.highorder_amplifier(foreign, highorder(), 1);
    });

    alignas(CoherentPipeline) unsigned char storage[sizeof(CoherentPipeline)];
    auto *first = new (storage) CoherentPipeline(1e8, 50., 2);
    const auto stale = first->seed({});
    first->~CoherentPipeline();
    auto *second = new (storage) CoherentPipeline(1e8, 50., 2);
    const auto valid = second->seed({});
    unchanged_failure<std::invalid_argument>(*second, valid, [&] {
        second->limited_amplifier(stale, limited());
    });
    unchanged_failure<std::invalid_argument>(*second, valid, [&] {
        second->highorder_amplifier(stale, highorder(), 1);
    });
    second->~CoherentPipeline();
}

void distortion_selection() {
    CoherentPipeline pipeline(1e8, 50., 2);
    const auto seed = pipeline.seed({source()});
    const auto distortion = pipeline.polynomial(seed, {0., 0., 1.}, 2).output;
    const auto zero = pipeline.linear_network({{distortion, 0}}, {0, 1}, 1, [](double) {
        return transfer(0.);
    });
    for (auto input : {distortion, zero}) {
        unchanged_failure<std::invalid_argument>(pipeline, input, [&] {
            pipeline.limited_amplifier(input, limited());
        });
        unchanged_failure<std::invalid_argument>(pipeline, input, [&] {
            pipeline.highorder_amplifier(input, highorder(), 1);
        });
        for (bool use_limited : {true, false}) {
            const auto result =
                use_limited
                    ? pipeline.limited_amplifier(input, limited(), propagated)
                    : pipeline.highorder_amplifier(input, highorder({1., 1.}), 2, propagated);
            require(result.input_power_w == pipeline.inspect(input).reduction.total_power_w &&
                        result.direct_contribution_count == 1 &&
                        result.nonlinear_counts.physical_input_count == 0 &&
                        result.nonlinear_counts.expanded_input_count == 0 &&
                        result.nonlinear_counts.generated_term_count == 0,
                    "conducted distortion entered the generating source set");
            require(pipeline.inspect(result.output).records.size() == 1 &&
                        pipeline.inspect(result.output).records[0].component.kind ==
                            SpectrumKind::harmonic,
                    "conducted distortion lost its kind or known zero identity");
        }
    }
}

void cutoff_counts_lo_and_zero() {
    CoherentPipeline pipeline(1e8, 50., 2);
    const auto input = pipeline.mix(pipeline.seed({source()}), {10, 2, 0., 0.});
    const auto all = pipeline.highorder_amplifier(input, highorder({1.}), 4);
    const auto direct = pipeline.highorder_amplifier(input, highorder({1.}), 2);
    const auto none = pipeline.highorder_amplifier(input, highorder({1.}), 1);
    require(all.direct_contribution_count == 2 && all.discarded_direct_contributions == 0 &&
                all.nonlinear_counts.generated_term_count == 4 &&
                all.nonlinear_counts.discarded_term_count == 0,
            "two mixed source paths should generate four positive quadratic paths");
    require(direct.direct_contribution_count == 2 && direct.discarded_direct_contributions == 0 &&
                direct.nonlinear_counts.generated_term_count == 4 &&
                direct.nonlinear_counts.discarded_term_count == 4 &&
                direct.nonlinear_counts.discarded_by_source_order.at(4) == 4 &&
                pipeline.inspect(direct.output).records.size() == 2,
            "LO factors were not counted at the nonlinear cutoff");
    require(none.direct_contribution_count == 2 && none.discarded_direct_contributions == 2 &&
                none.nonlinear_counts.generated_term_count == 4 &&
                none.nonlinear_counts.discarded_term_count == 4 &&
                pipeline.inspect(none.output).records.empty(),
            "all direct histories must be deliberately omitted below their source order");
    require(all.input_power_w == direct.input_power_w &&
                direct.input_power_w == none.input_power_w && all.input_power_w > 0. &&
                all.operating_point.fundamental_amplitude_gain ==
                    none.operating_point.fundamental_amplitude_gain &&
                all.operating_point.nonlinear_input_scale ==
                    none.operating_point.nonlinear_input_scale,
            "cutoff changed the full incoming physical drive");
    const auto silent = pipeline.mix(pipeline.seed({source(100, 1, {})}), {10, 2, 0., 0.});
    const auto dropped_zero = pipeline.highorder_amplifier(silent, highorder(), 1);
    require(dropped_zero.direct_contribution_count == 2 &&
                dropped_zero.discarded_direct_contributions == 2 &&
                dropped_zero.nonlinear_counts.expanded_input_count == 2 &&
                pipeline.inspect(dropped_zero.output).records.empty(),
            "zero direct monomials must be counted by the cutoff");
}

// Keep only the upper sideband after short blocks of mixers. The degree-one
// polynomial removes the explicit zero paths left by the linear filter without
// changing the retained source kind. This reaches the public 256-factor boundary
// within the ordinary operation and cumulative storage limits.
CoherentPipeline::Stream source_with_factors(CoherentPipeline &pipeline, std::size_t factors) {
    auto stream = pipeline.seed({source(1024)});
    int bin = 1024;
    std::size_t completed = 1;
    while (completed < factors) {
        const auto block = std::min<std::size_t>(8, factors - completed);
        for (std::size_t i = 0; i < block; ++i) {
            stream = pipeline.mix(stream, {1, 2, 0., 0.});
            ++bin;
        }
        completed += block;
        stream = pipeline.linear_network({{stream, 0}}, {0, 1}, 1, [bin](double frequency) {
            return transfer(frequency == bin * 1e8 ? 1. : 0.);
        });
        stream = pipeline.polynomial(stream, {0., 1.}, 256).output;
    }
    require(pipeline.inspect(stream).records.size() == 1 &&
                pipeline.inspect(stream).records[0].component.kind == SpectrumKind::source &&
                pipeline.inspect(stream).records[0].history[0].factors.size() == factors,
            "high-factor source fixture is invalid");
    return stream;
}

void hard_factor_limit_and_filter_order() {
    CoherentPipeline pipeline(1e8, 50., 2);
    const auto input = source_with_factors(pipeline, 256);
    unchanged_failure<std::length_error>(pipeline, input, [&] {
        pipeline.limited_amplifier(input, limited());
    });
    const auto retained = pipeline.highorder_amplifier(input, highorder({1., 1.}), 256);
    require(retained.direct_contribution_count == 1 &&
                retained.discarded_direct_contributions == 0 &&
                retained.nonlinear_counts.generated_term_count == 3 &&
                retained.nonlinear_counts.discarded_term_count == 3 &&
                retained.nonlinear_counts.discarded_by_source_order.at(512) == 1 &&
                retained.nonlinear_counts.discarded_by_source_order.at(768) == 2 &&
                pipeline.inspect(retained.output).records[0].history[0].factors.size() == 256,
            "high-order cutoff did not precede the hard factor composition limit");
    const auto removed = pipeline.highorder_amplifier(input, highorder(), 255);
    require(removed.discarded_direct_contributions == 1 &&
                removed.input_power_w == retained.input_power_w &&
                pipeline.inspect(removed.output).records.empty(),
            "direct 256-factor boundary did not apply to the complete history");
    const auto direct_limited =
        pipeline.limited_amplifier(pipeline.polynomial(input, {0., 0., 1.}, 256).output, limited());
    require(pipeline.inspect(direct_limited.output).records.empty(),
            "explicitly empty polynomial output was not accepted");
}

void expanded_and_active_limits() {
    CoherentPipeline boundary(1e8, 50., 5000);
    std::vector<CoherentComponent> sources;
    for (std::uint64_t group = 1; group <= 4096; ++group) {
        sources.push_back(source(100, group, {.00001, 0.}));
    }
    const auto input = boundary.seed(sources);
    const auto empty_coefficients = boundary.highorder_amplifier(input, highorder(), 1);
    const auto zero_coefficients =
        boundary.highorder_amplifier(input, highorder({0., 0., 0.}), 256);
    for (const auto *result : {&empty_coefficients, &zero_coefficients}) {
        require(result->nonlinear_counts.physical_input_count == 4096 &&
                    result->nonlinear_counts.expanded_input_count == 4096 &&
                    result->nonlinear_counts.generated_term_count == 0 &&
                    result->direct_contribution_count == 4096 &&
                    boundary.inspect(result->output).records.size() == 4096,
                "all-zero coefficients changed the accepted flattening boundary");
    }
    unchanged_failure<std::length_error>(boundary, input, [&] {
        boundary.limited_amplifier(input, limited());
    });
    unchanged_failure<std::length_error>(boundary, input, [&] {
        boundary.highorder_amplifier(input, highorder({1.}), 1);
    });

    CoherentPipeline expanded(1e8, 50., 2);
    sources.clear();
    for (int bin = 10000; bin < 11024; ++bin) {
        sources.push_back(source(bin, 1, {}));
    }
    auto too_many = expanded.seed(sources);
    too_many = expanded.mix(too_many, {1, 2, 0., 0.});
    too_many = expanded.mix(too_many, {1, 2, 0., 0.});
    too_many = expanded.mix(too_many, {2, 2, 0., 0.});
    std::size_t history_count = 0;
    for (const auto &record : expanded.inspect(too_many).records) {
        history_count += record.history.size();
    }
    require(history_count > 4096 && expanded.inspect(too_many).records.size() <= 4096,
            "expanded-history fixture must exceed only the flattened input bound");
    unchanged_failure<std::length_error>(expanded, too_many, [&] {
        expanded.highorder_amplifier(too_many, highorder(), 1);
    });
    unchanged_failure<std::length_error>(expanded, too_many, [&] {
        expanded.limited_amplifier(too_many, limited());
    });

    CoherentPipeline histories(1e8, 50., 2);
    std::vector<CoherentPipeline::PortStream> branches;
    for (int i = 0; i < 65; ++i) {
        branches.push_back(
            {histories.mix(histories.seed({source(100 + i)}), {90 + i, 2, 0., 0.}), 0});
    }
    const auto combined = histories.linear_network(branches, {0, 1}, 1, [](double frequency) {
        return transfer(frequency == 10e8 ? 1. : 0.);
    });
    require(histories.inspect(combined).records.front().history.size() == 65,
            "active-history fixture failed to merge its physical IF root");
    unchanged_failure<std::length_error>(histories, combined, [&] {
        histories.limited_amplifier(combined, limited());
    });
    unchanged_failure<std::length_error>(histories, combined, [&] {
        histories.highorder_amplifier(combined, highorder({1.}), 1);
    });
}

void candidate_premerge_and_work_limits() {
    // 64 genuinely active sources and exactly 4096 native RF candidates are
    // accepted. The semantic cutoff removes candidates only after enumeration.
    CoherentPipeline boundary(1e8, 50., 64);
    std::vector<CoherentComponent> exact;
    for (int i = 0; i < 64; ++i) {
        exact.push_back(source(100 + i, static_cast<std::uint64_t>(i + 1)));
    }
    const auto exact_input = boundary.seed(exact);
    const auto exact_result = boundary.highorder_amplifier(exact_input, highorder({1.}), 1);
    require(exact_result.nonlinear_counts.expanded_input_count == 64 &&
                exact_result.nonlinear_counts.generated_term_count == 4096 &&
                exact_result.nonlinear_counts.discarded_term_count == 4096 &&
                boundary.inspect(exact_result.output).records.size() == 64,
            "64-active/4096-candidate boundary was not accepted before source cutoff");
    CoherentPipeline candidates(1e8, 50., 64);
    std::vector<CoherentComponent> tones;
    for (int i = 0; i < 18; ++i) {
        tones.push_back(source(100 + i, static_cast<std::uint64_t>(i + 1)));
    }
    const auto input = candidates.seed(tones);
    unchanged_failure<std::length_error>(candidates, input, [&] {
        candidates.limited_amplifier(input, limited());
    });
    unchanged_failure<std::length_error>(candidates, input, [&] {
        candidates.highorder_amplifier(input, highorder({0., 0., 1.}), 1);
    });

    CoherentPipeline premerge(1e8, 50., 4096);
    tones.clear();
    for (std::uint64_t group = 1; group <= 4096; ++group) {
        tones.push_back(source(100, group, group == 1 ? Complex{.01, 0.} : Complex{}));
    }
    const auto full = premerge.seed(tones);
    unchanged_failure<std::length_error>(premerge, full, [&] {
        premerge.limited_amplifier(full, limited());
    });
    unchanged_failure<std::length_error>(premerge, full, [&] {
        premerge.highorder_amplifier(full, highorder({1.}), 2);
    });
    const auto cutoff = premerge.highorder_amplifier(full, highorder({1.}), 1);
    require(cutoff.nonlinear_counts.generated_term_count == 1 &&
                cutoff.nonlinear_counts.discarded_term_count == 1 &&
                premerge.inspect(cutoff.output).records.size() == 4096,
            "discarded candidates incorrectly consumed the pre-merge output budget");

    // The documented bound is pre-merge: conducted and newly generated H2/H3
    // have matching identities, so canonical reconciliation would fit, but the
    // raw retained input to store still must not exceed 4096 records.
    for (bool use_limited : {true, false}) {
        CoherentPipeline merging(1e8, 50., 4096);
        tones.clear();
        const std::uint64_t count = use_limited ? 4093 : 4095;
        for (std::uint64_t group = 1; group <= count; ++group) {
            tones.push_back(source(100, group, group == 1 ? Complex{.01, 0.} : Complex{}));
        }
        const auto seed = merging.seed(tones);
        const auto first = use_limited ? merging.limited_amplifier(seed, limited())
                                       : merging.highorder_amplifier(seed, highorder({1.}), 2);
        require(merging.inspect(first.output).records.size() == 4096,
                "pre-merge reconciliation fixture is not at the record boundary");
        unchanged_failure<std::length_error>(merging, first.output, [&] {
            if (use_limited) {
                merging.limited_amplifier(first.output, limited(), propagated);
            } else {
                merging.highorder_amplifier(first.output, highorder({1.}), 2, propagated);
            }
        });
    }

    CoherentPipeline work(1e8, 50., 64);
    tones.clear();
    for (std::uint64_t group = 1; group <= 64; ++group) {
        tones.push_back(source(100, group, {1e-100, 0.}));
    }
    const auto small = work.seed(tones);
    std::vector<double> coefficients(10, 0.);
    coefficients.back() = 1.;
    bool work_limit = false;
    unchanged_failure<std::length_error>(work, small, [&] {
        try {
            work.highorder_amplifier(small, highorder(coefficients), 1);
        } catch (const std::length_error &error) {
            work_limit = std::string(error.what()).find("enumeration work") != std::string::npos;
            throw;
        }
    });
    require(work_limit, "zero candidate amplitudes or low cutoff bypassed native work limit");
}

void arithmetic_and_group_rollback() {
    CoherentPipeline frequency(1., 50., 2);
    const auto high = frequency.seed({source(std::numeric_limits<int>::max())});
    unchanged_failure<std::overflow_error>(frequency, high, [&] {
        frequency.limited_amplifier(high, limited());
    });
    unchanged_failure<std::overflow_error>(frequency, high, [&] {
        frequency.highorder_amplifier(high, highorder({1.}), 1);
    });
    CoherentPipeline support(1e8, 50., 2);
    auto left = source(100), right = source(101, 2);
    left.bandwidth_hz = right.bandwidth_hz = 2e8;
    const auto wide = support.seed({left, right});
    unchanged_failure<std::invalid_argument>(support, wide, [&] {
        support.limited_amplifier(wide, limited());
    });
    unchanged_failure<std::invalid_argument>(support, wide, [&] {
        support.highorder_amplifier(wide, highorder({1.}), 1);
    });
    CoherentPipeline coefficient(1e8, 50., 2);
    const auto empty = coefficient.seed({});
    unchanged_failure<std::overflow_error>(coefficient, empty, [&] {
        coefficient.highorder_amplifier(empty, highorder({1e308}), 1);
    });

    // Nonzero paths behind a physically canceled IF still have to be scaled.
    // Their intermediate overflow must fail, rather than taking a zero-drive
    // shortcut that erases the known histories.
    CoherentPipeline cancellation(1e8, 50., 2);
    const auto cancel_rf =
        cancellation.seed({source(99, 1, {1e153, 0.}), source(101, 1, {-1e153, 0.})});
    const auto mixed = cancellation.mix(cancel_rf, {100, 2, 0., 0.});
    const auto canceled =
        cancellation.linear_network({{mixed, 0}}, {0, 1}, 1, [](double frequency) {
            return transfer(frequency == 1e8 ? 1. : 0.);
        });
    require(cancellation.inspect(canceled).reduction.total_power_w == 0.,
            "overflow fixture did not cancel physically");
    unchanged_failure<std::invalid_argument>(cancellation, canceled, [&] {
        cancellation.limited_amplifier(canceled, {40., 100., 103., 50., 50., 50.});
    });
    unchanged_failure<std::invalid_argument>(cancellation, canceled, [&] {
        cancellation.highorder_amplifier(canceled, {40., 100., 103., {}, 50.}, 256);
    });

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    CoherentPipeline exhausted(1e8, 50., maximum);
    const auto zero = exhausted.seed({source(100, 1, {})});
    unchanged_failure<std::overflow_error>(exhausted, zero, [&] {
        exhausted.limited_amplifier(zero, limited());
    });
    unchanged_failure<std::overflow_error>(exhausted, zero, [&] {
        exhausted.highorder_amplifier(zero, highorder(), 1);
    });
    CoherentPipeline attempted(1e8, 50., maximum - 3), clean(1e8, 50., maximum - 3);
    const auto attempted_input = attempted.seed({source()});
    const auto clean_input = clean.seed({source()});
    unchanged_failure<std::overflow_error>(attempted, attempted_input, [&] {
        attempted.limited_amplifier(attempted_input, limited());
    });
    const auto actual = attempted.highorder_amplifier(attempted_input, highorder({1.}), 2);
    const auto expected = clean.highorder_amplifier(clean_input, highorder({1.}), 2);
    same_snapshot(attempted.inspect(actual.output), clean.inspect(expected.output));
    require(same_usage(attempted.usage(), clean.usage()),
            "failed group allocations changed subsequent deterministic allocation");
}

void context_and_cumulative_limits() {
    CoherentPipeline operations(1e8, 50., 2);
    const auto empty = operations.seed({});
    for (int i = 0; i < 512; ++i) {
        if (i % 2 == 0) {
            operations.limited_amplifier(empty, limited());
        } else {
            operations.highorder_amplifier(empty, highorder(), 1);
        }
    }
    require(operations.usage().transformation_count == 512 &&
                operations.usage().stream_count == 513,
            "amplifier consumed more than one transformation or stream");
    unchanged_failure<std::length_error>(operations, empty, [&] {
        operations.limited_amplifier(empty, limited());
    });
    unchanged_failure<std::length_error>(operations, empty, [&] {
        operations.highorder_amplifier(empty, highorder(), 1);
    });
    CoherentPipeline streams(1e8, 50., 2);
    const auto first = streams.seed({});
    for (int i = 1; i < 4096; ++i) {
        streams.seed({});
    }
    unchanged_failure<std::length_error>(streams, first, [&] {
        streams.limited_amplifier(first, limited());
    });
    unchanged_failure<std::length_error>(streams, first, [&] {
        streams.highorder_amplifier(first, highorder(), 1);
    });

    CoherentPipeline records(1e8, 50., 4096);
    std::vector<CoherentComponent> many;
    for (std::uint64_t group = 1; group <= 4096; ++group) {
        many.push_back(source(100, group, {}));
    }
    const auto full = records.seed(many);
    for (int i = 0; i < 15; ++i) {
        records.highorder_amplifier(full, highorder(), 1);
    }
    require(records.usage().physical_record_count == 65536 &&
                records.usage().history_term_count == 65536,
            "cumulative record/history limit boundary incorrect");
    unchanged_failure<std::length_error>(records, full, [&] {
        records.limited_amplifier(full, limited());
    });
    unchanged_failure<std::length_error>(records, full, [&] {
        records.highorder_amplifier(full, highorder(), 1);
    });

    CoherentPipeline factors(1e8, 50., 16);
    std::vector<CoherentPipeline::PortStream> branches;
    for (std::uint64_t group = 1; group <= 16; ++group) {
        auto stream = factors.seed({source(8, group, {.2, 0.})});
        for (int i = 0; i < 8; ++i) {
            stream = factors.polynomial(stream, {0., 0., 1.}, 256).output;
        }
        branches.push_back({stream, 0});
    }
    const auto long_histories = factors.linear_network(branches, {0, 1}, 1, [](double) {
        return transfer();
    });
    const auto conducted = factors.limited_amplifier(long_histories, limited(), propagated);
    require(conducted.direct_contribution_count == 16 &&
                conducted.nonlinear_counts.expanded_input_count == 0 &&
                factors.inspect(conducted.output).records[0].history[0].factors.size() == 256,
            "limited conduction rejected an existing representable 256-factor history");
    while (1048576 - factors.usage().history_factor_count >= 4096) {
        factors.highorder_amplifier(long_histories, highorder(), 256, propagated);
    }
    require(factors.usage().history_factor_count == 1048560,
            "cumulative history factor boundary incorrect");
    unchanged_failure<std::length_error>(factors, long_histories, [&] {
        factors.limited_amplifier(long_histories, limited(), propagated);
    });
    unchanged_failure<std::length_error>(factors, long_histories, [&] {
        factors.highorder_amplifier(long_histories, highorder(), 256, propagated);
    });
}

void allocation_failure_atomicity() {
    const auto low_model = limited();
    const auto high_model = highorder({1., 1.});
    const std::vector<double> coefficients{0., 1., 1., 1.};
    const char *names[]{
        "polynomial allocation sweep", "limited allocation sweep", "high-order allocation sweep"};
    for (int operation = 0; operation < 3; ++operation) {
        allocation_failure::phase = names[operation];
        const auto apply = [&](CoherentPipeline &pipeline, CoherentPipeline::Stream input) {
            if (operation == 0) {
                return pipeline.polynomial(input, coefficients, 2).output;
            }
            if (operation == 1) {
                return pipeline.limited_amplifier(input, low_model).output;
            }
            return pipeline.highorder_amplifier(input, high_model, 2).output;
        };
        CoherentPipeline control(1e8, 50., 2);
        const auto control_input = control.seed({source()});
        const auto expected = apply(control, control_input);
        std::size_t failures = 0;
        for (std::ptrdiff_t allowance = 0;; ++allowance) {
            CoherentPipeline pipeline(1e8, 50., 2);
            const auto input = pipeline.seed({source()});
            const auto usage = pipeline.usage();
            const auto *address = &pipeline.inspect(input);
            const auto saved = *address;
            bool failed = false;
            allocation_failure::allowance = allowance;
            allocation_failure::remaining = allowance;
            try {
                apply(pipeline, input);
            } catch (const std::bad_alloc &) {
                failed = true;
            } catch (...) {
                allocation_failure::remaining = -1;
                throw;
            }
            allocation_failure::remaining = -1;
            if (!failed) {
                require(same_usage(pipeline.usage(), control.usage()),
                        "successful injected operation committed wrong usage");
                break;
            }
            ++failures;
            require(same_usage(usage, pipeline.usage()), "allocation failure committed usage");
            require(address == &pipeline.inspect(input), "allocation failure moved input snapshot");
            same_snapshot(saved, pipeline.inspect(input));
            // This also proves that the mutation guard reset and no temporary
            // source, history, identity or group allocation escaped the failure.
            const auto recovered = apply(pipeline, input);
            same_snapshot(pipeline.inspect(recovered), control.inspect(expected));
            require(same_usage(pipeline.usage(), control.usage()),
                    "allocation rollback changed subsequent deterministic allocation");
        }
        require(failures > 20, "allocation failure sweep did not reach nontrivial work");
        std::fprintf(stderr, "%s: passed %zu allocation failures\n", names[operation], failures);
    }
}

} // namespace

int main() {
    // Report failures rather than entering a platform CRT crash/report dialog.
    // The handler uses no C++ allocation and remains usable inside a fault sweep.
    std::set_terminate([]() noexcept {
        std::fprintf(stderr,
                     "terminate: %s, allocation allowance %td, remaining %td\n",
                     allocation_failure::phase,
                     allocation_failure::allowance,
                     allocation_failure::remaining);
        std::fflush(stderr);
        std::_Exit(1);
    });

    struct Case {
        const char *name;
        void (*run)();
    };

    const Case cases[]{{"empty/silent/validation", empty_silent_and_validation},
                       {"distortion selection", distortion_selection},
                       {"cutoff/LO/zero counts", cutoff_counts_lo_and_zero},
                       {"hard-factor limit/filter ordering", hard_factor_limit_and_filter_order},
                       {"expanded/active limits", expanded_and_active_limits},
                       {"candidate/premerge/work limits", candidate_premerge_and_work_limits},
                       {"arithmetic/group rollback", arithmetic_and_group_rollback},
                       {"context/cumulative limits", context_and_cumulative_limits},
                       {"allocation failure atomicity", allocation_failure_atomicity}};
    for (const auto &test : cases) {
        allocation_failure::phase = test.name;
        allocation_failure::allowance = -1;
        std::fprintf(stderr, "RUN %s\n", test.name);
        std::fflush(stderr);
        try {
            test.run();
        } catch (const std::exception &error) {
            allocation_failure::remaining = -1;
            std::fprintf(stderr,
                         "FAIL %s, allocation allowance %td: %s\n",
                         allocation_failure::phase,
                         allocation_failure::allowance,
                         error.what());
            return 1;
        }
        std::fprintf(stderr, "PASS %s\n", test.name);
    }
}
