# Linear external S extraction: one factorization per frequency

`LinearNetwork::external_s(ports)` now assembles and factors `I-C*S` once, then
solves only the selected unit-excitation columns. For n network ports and m
external ports the dense work is O(n³ + mn²), with O(n²) working storage plus
the m-by-m result. There is no cross-frequency cache, n-RHS inverse, sparse
solver, noise refactor, or change to physical reference coordinates.

The private factorization records each row pivot and its elimination factors.
Earlier lower-column entries are elimination history, rather than the usual
row-permuted L factor. Each RHS replays the recorded swap and eliminations in
the same order as `solve()`. The original dense matrix is retained to check each
RHS residual. All outgoing ports, including internal ports, retain finite-value
checks. Pivot threshold, relative residual threshold (1e-10), exception messages,
and coefficient assembly follow the existing solver. `solve()` itself is
unchanged, providing a direct legacy extraction reference without a shared new
factorization masking regressions.

Existing extraction preconditions remain: nonempty, unique, in-range, unassigned
external ports; all internal ports assigned; zero independent sources. External
ordering defines matrix rows/columns, and extraction leaves the network unchanged.
Complex power-wave connections retain their reflection and coupling terms.

## Focused verification

`external_s_factorization_test` uses always-active `require`/`near` checks, including
in Release builds. It covers:

- The old copy/terminate/solve extraction algorithm on forty deterministic dense,
  nonreciprocal complex-feedback networks and multiple external orderings
- Independent physical V/I equations with different complex port references
- Direct S extraction, single external port, mutation and validation checks
- Singular and rejected near-singular feedback, plus accepted close feedback
- Coefficient overflow, second-RHS response overflow, and overflow confined to an
  internal outgoing port even when selected external outputs are finite

Existing external-S cascade/reflection analytic tests and linear/mixed-reference
regressions remain applicable. These focused checks do not establish cross-platform
CI success, vendor equivalence, or full-suite acceptance for this change.

## Reproducible performance comparison

From the repository root, with a writable workspace build directory:

```sh
BUILD_DIR="${BUILD_DIR:-$PWD/build-external-s}"
mkdir -p "$BUILD_DIR/tmp"
export TMPDIR="$BUILD_DIR/tmp"
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -O3 -DNDEBUG -Iinclude \
    benchmarks/linear_external_s.cpp \
    -o "$BUILD_DIR/linear_external_s_benchmark"
"$BUILD_DIR/linear_external_s_benchmark"
```

The benchmark runs the old per-port copy/terminate/`solve()` algorithm and the
new extraction on identical dense complex S matrices, reference impedances,
connections and selected external ports. It first checks entrywise agreement,
then records seven timing samples per path, alternating execution order after a
warmup. It reports the median, not the best sample. Every result contributes to a
printed checksum. There is deliberately no timing pass/fail threshold in CTest.

Local observation on 2026-10-14, Linux x86_64, GCC 14.2.0, `-O3 -DNDEBUG`:

| Network / external ports | Old median ms | Factored median ms | Ratio |
|---|---:|---:|---:|
| 96 / 32 (dense, 64 internal ports) | 36.917249 | 3.446269 | 10.71× |
| 160 / 2 (158 internal ports) | 9.699459 | 4.920278 | 1.97× |

Both comparisons had zero observed maximum entrywise difference. These are
single-machine measurements over seven samples, not portable performance
promises. The two-external case specifically avoids treating every internal port
as a requested RHS. Raw samples are in
[`validation/linear-external-s-benchmark-20261014.csv`](../validation/linear-external-s-benchmark-20261014.csv).
