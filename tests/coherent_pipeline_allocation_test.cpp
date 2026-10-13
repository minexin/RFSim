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

// MSVC checked iterators allocate proxies even in noexcept vector constructors.
// Fault injection there terminates inside the STL before an exception can reach
// RFModel. This dedicated target uses the normal throwing-allocation contract;
// ordinary Debug physics/limits targets keep their default checked iterators.
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
#error "The allocation-failure target requires _ITERATOR_DEBUG_LEVEL=0 on MSVC"
#endif

using namespace rfmodel;

namespace {
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

void allocation_failure_atomicity() {
    const auto low_model = limited();
    const auto high_model = highorder({1., 1.});
    const std::vector<double> coefficients{0., 1., 1., 1.};
    const char *names[]{"polynomial allocation sweep",
                        "limited allocation sweep",
                        "high-order allocation sweep",
                        "mixer-bank allocation sweep",
                        "network-batch allocation sweep"};
    for (int operation = 0; operation < 5; ++operation) {
        allocation_failure::phase = names[operation];
        const auto apply = [&](CoherentPipeline &pipeline, CoherentPipeline::Stream input) {
            if (operation == 0) {
                return pipeline.polynomial(input, coefficients, 2).output;
            }
            if (operation == 1) {
                return pipeline.limited_amplifier(input, low_model).output;
            }
            if (operation == 2) {
                return pipeline.highorder_amplifier(input, high_model, 2).output;
            }
            if (operation == 3) {
                return pipeline.mixer_bank({{input, {101, 2, 0., .2}}, {input, {101, 2, 0., .4}}})
                    .front();
            }
            return pipeline
                .linear_network_outputs({{input, 0}},
                                        {0, 1, 2},
                                        {1, 2},
                                        [](double) {
                                            LinearNetwork network;
                                            network.add({3, {0., 0., 0., .5, 0., 0., -.5, 0., 0.}});
                                            return network;
                                        })
                .front();
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
    // No C++ allocation: remain usable when a fault reaches a noexcept boundary.
    std::set_terminate([]() noexcept {
        std::fprintf(stderr,
                     "terminate: %s, allocation allowance %td, remaining %td\n",
                     allocation_failure::phase,
                     allocation_failure::allowance,
                     allocation_failure::remaining);
        std::fflush(stderr);
        std::_Exit(1);
    });
#if defined(_MSC_VER)
    std::fprintf(
        stderr, "MSVC allocation sweep: _ITERATOR_DEBUG_LEVEL=%d\n", _ITERATOR_DEBUG_LEVEL);
#endif
    try {
        allocation_failure_atomicity();
    } catch (const std::exception &error) {
        allocation_failure::remaining = -1;
        std::fprintf(stderr,
                     "FAIL %s, allocation allowance %td: %s\n",
                     allocation_failure::phase,
                     allocation_failure::allowance,
                     error.what());
        return 1;
    }
}
