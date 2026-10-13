# Developer-only native reference replay

> 接口更新（2026-10-13）：Python 产品包、wheel 和 Python/JSON CLI 已退役。本页涉及这些入口的命令与验证属于历史记录；当前 C++ 入口及迁移范围见 [退役记录](python-interface-retirement.md)。保留的物理结论与厂商差异不因退役而改变。

This bounded replay removes the product `rfmodel` import from five existing
high-order reference suites. The 2026-10-13 extension also restores the A01
AM/PM diagnosis through `LegacyPointDerivativeAmPm`. Numerical work calls five existing C++
kernels: intercept coefficient conversion, `CoherentLimitedAmplifier` operating
point, `CoherentPolynomial`, `CoherentHighOrderAmplifier`, and the AM/PM response. There are no new RF
equations, C ABI changes, JSON model parser, or replacement Python product binding.
The standard-library Python adapter handles only transport, immutable data
records, archived JSON, existing independent scoring and provenance.

The header-only SDK and its installed/exported targets have no probe or Python
runtime dependency. `RFMODEL_BUILD_REFERENCE_TESTS` defaults to OFF; enabling it
explicitly requires `BUILD_TESTING` and Python >=3.9, independently of both the
C ABI and binding-test options. Missing Python fails rather than skips this gate.

## Build and verify

```sh
cmake -S . -B build-reference -DCMAKE_BUILD_TYPE=Release \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_PYTHON_TESTS=OFF \
  -DRFMODEL_BUILD_REFERENCE_TESTS=ON
cmake --build build-reference --config Release \
  --target rfmodel_reference_highorder_probe rfmodel_reference_highorder_oracle
ctest --test-dir build-reference -C Release -R '^native_' --output-on-failure
```

Multi-configuration generators put the executables and their manifests under
`Release/` or `Debug/`. An exported source tree without `.git` may provide its known
base revision using `-DRFMODEL_REFERENCE_SOURCE_REVISION=<revision>`; otherwise the
manifest says `unavailable`. Actual configured source hashes, not that revision
label alone, identify changed/exported sources.

Each of the six suites accepts `--native-probe <executable>`; the five historical
high-order suites retain their old positional argument only to report retirement.
The AM/PM scorer requires the explicit native probe. Native
runs use `python -I -S`, install an active import finder that rejects `rfmodel`
and every submodule before importing scorers, and check no product module entered
`sys.modules`. The protocol suite also stages the reference scripts and required
immutable archives without any `python/rfmodel` tree, then runs all six suites
in fresh isolated processes. The independent C++ oracle calls the five kernels
directly and returns complete records for transport comparisons; archived vendor
samples and existing independent physics tests remain the numerical oracles.

`native_replay_installed_sdk` separately configures the all-off SDK, installs and
relocates it, copies the probe and direct oracle into an independent consumer,
and builds through only `find_package(RFModel CONFIG REQUIRED)` and
`RFModel::rfmodel`. It checks for leaked developer/Python/shared-runtime files and
runs the protocol and staged archive suites with the installed-header build.

## Frozen evidence semantics

The original capture/report bytes are never modified. Replay preserves:

- Seven measured-input configurations and exactly 1,413 scored order-4..11
  holdouts; designated odd calibration observations are excluded; 10 unrecorded
  terms remain explicit rather than becoming zero observations.
- Nominal-source even-order statuses `[true, true, false, false]` for orders
  4/6/8/10 at the unchanged `1e-7` wave tolerance. The archived order-8 residual
  `1.0000000560950184e-7` is just above the threshold; no tolerance adjustment or
  relabeling is permitted to hide arithmetic/serialization differences.
- Odd holdout counts 68/136/235/368 for orders 5/7/9/11, and the existing phase,
  calibration, source-hash, voltage, bandwidth, freshness and omission faults.
- Fourth/fifth-order missing spectra, fitted fifth-order coefficient,
  spectrum-reduction 224 observations, 44 predictions/case, restored counts
  12/14/14, and the rejected full-group-sum hypothesis.
- `diagnostic_only` and `eligible_for_compatibility=false`. Measured source waves
  and fitted odd coefficients remain conditional local evidence, not full vendor
  network prediction or automatic RFAMP coefficient equivalence.

Unmigrated numerical comparisons explicitly report that the retired binding
cannot perform a comparison. Cascade capture validation is importable, but its
combined-origin scorer has not been migrated. The new typed coherent
pipeline now has bounded limited/high-order model adapters and explicit distortion
propagation, but the cascade scorer has not been migrated to these APIs. The
archived cascade result remains 230 checks with 22 failures, including the known
0 dB discrepancy; adding adapters alone does not change that evidence. See the
[adapter contract](coherent-pipeline-amplifiers.md). The four high-order operations
and their archived scoring path remain unchanged. Python product retirement is
recorded separately; these developer tools do not replace a product binding.
No new SystemVue simulation is performed or claimed.

The A01 extension retains 7/37 matching PointDerivative observations and 144/148
matching retrospective DataPoints candidates at `1e-5 degree`; both full sets fail.
Its separate report includes 37 native exchanges, build/source/compiler identity,
executable digest and optional local raw-capture verification. See
[the A01 replay record](systemvue-am-pm-existing-reference.md). Direct-oracle cases
cover 45 combinations of peak sign, reference power, zero drive and both branches;
these test transport, not new vendor compatibility.

## Protocol 1

One disposable process accepts one ASCII request on stdin, with exact EOF. Tokens
are separated by ordinary ASCII whitespace, including CRLF. There are no paths,
callbacks, arbitrary strings or JSON values in the payload.

Header: `RFMODEL_NATIVE_REPLAY 1 OP REQUEST_ID`, where `REQUEST_ID` is 64 lowercase
hex characters. The Python adapter derives it from canonical operation/payload
bytes. Responses echo it and the embedded 64-hex build-manifest ID.

| Operation | Positional request payload | Success payload |
|---|---|---|
| `intercepts` | gain_dB Z count; count rows `(k1 k2 intercept_dBm sign reference)` | coefficient count, full `c0..cn` |
| `operating_point` | drive_W gain_dB OP1dB_dBm OPSAT_dBm IIP2_dBm IIP3_dBm Z | five operating-point fields |
| `polynomial` | spacing_Hz Z reserved_group coefficient_count full `c0..cn` input_count components | input_count reduced inputs, term_count terms |
| `highorder` | spacing_Hz Z reserved_group gain_dB OP1dB_dBm OPSAT_dBm coefficient_count `a2..an` input_count components | total_input_W, five operating-point fields, polynomial-shaped records |
| `am_pm` | output_power_W peak_degree_per_dB output_reference_dBm | phase_rad radial_phase_slope |

Component: `positive_int32_bin kind bandwidth_Hz uint64_group real imag`, with
complex amplitude in sqrt(W). This gate accepts at most two source-kind inputs
(kind 0) and does not support distortion propagation. Returned kinds are 0/1/2
(source/harmonic/intermod); reduction and term order are retained exactly. Group
0 is forbidden for components; reserved group 0 is valid. Intercept reference is
0=input or 1=output; coefficient sign is -1 or +1. Kernel domain checks remain.

Term: `order`, exactly `order` nonzero signed one-based reduced-input indices,
then its component. Operating-point fields are fundamental amplitude gain,
nonlinear input scale, limited input W, quadratic and cubic voltage coefficients.
Full polynomial coefficient count is 1..12; explicit high-order count is 0..10;
intercept count is 0..10; output term count is at most 4,096.

Success header: `RFMODEL_NATIVE_REPLAY 1 OP REQUEST_ID BUILD_ID ok`, followed by
one complete payload. Error header:
`RFMODEL_NATIVE_REPLAY 1 error REQUEST_ID BUILD_ID STATUS`, with no payload.
If request identity cannot yet be parsed, its field is 64 zeros. Exit codes and
statuses are 2=`invalid_input`, 3=`resource_limit`, 4=`numeric_range`,
5=`internal_error`; success exits 0. Errors are deterministic categories, not
platform-dependent exception messages. A complete result is validated before
any success bytes are emitted. Extra/truncated fields, wrong identities,
nonfinite values, invalid indices or duplicate terms cannot become comparison
passes. There is no cached or binding fallback.

Request limit: 64 KiB. Response limit: 16 MiB. Token limit: 128 bytes. The adapter
also bounds stderr to 64 KiB and subprocess time to 10 seconds by default
(configurable in `(0,60]`). Checked decimal integer conversion rejects overflow.
Real tokens use strict decimal syntax; booleans, NaN, infinity, hex floats and
malformed exponents are rejected. Finite subnormal doubles are permitted;
nonzero literals rounding to zero are rejected as `numeric_range`. C++ parsing
and output use the classic locale and `max_digits10` double precision.

## Provenance and new reports

```sh
python -I -S scripts/reference/native_replay.py \
  --probe build-reference/rfmodel_reference_highorder_probe \
  --report /new/path/native-replay.json
```

The build emits `<probe>.manifest.json` alongside the executable. Its content
identity includes all public C++ headers, reference scripts, probe/oracle,
relevant CMake build logic, compiler/version/flags and configuration. Rebuilding
after these files change refreshes the embedded identity. The adapter checks
that response identity agrees with the sidecar, hashes both executable and
sidecar, and rejects changes during a replay. The sidecar uses a canonical LF
byte envelope: the embedded ID is SHA-256 of the exact identity body, and the
adapter verifies that hash before trusting its fields. Changing metadata while
retaining the ID is rejected. This is reproducibility bookkeeping and corruption
detection, not a signed build attestation or independent proof that a compiler
used the claimed flags. Installed-consumer identity hashes the actual relocated
installed headers and copied probe/oracle bytes; it does not reuse source-tree
header hashes as evidence of installed contents.

A new report records available archive byte hashes, source/build identity,
scorer/adapter hashes, canonical requests and request/response digests, units,
calibration/holdout roles, recomputed diagnostic reports and the unified-model
test. Historical `library_sha256` values remain historical; the executable has
a separately named `executable_sha256`. Embedded raw-capture/workspace hashes
are preserved as historical provenance, not presented as fresh verification of
missing original files. A report is published atomically to a new destination
only after successful validation; existing paths, input archives, scorer files,
probe and manifest cannot be overwritten. A failure leaves existing files alone.

Repeated requests to the same build must yield byte-identical responses. Across
platforms, existing numerical tolerances and exact structural/status invariants
apply; bitwise cross-platform floating-point identity is not promised. The CI
matrix registers Linux/macOS/Windows Debug and Release developer replay jobs;
only completed runs on the resulting revision count as platform evidence.

## Local verification snapshot (2026-10-13)

On Linux/GCC 14.2.0, the isolated source export based on `3a44a733` passed
Debug 102/102 and Release 102/102 CTests with the C ABI disabled, including
27 protocol/archive/import tests and the installed/relocated SDK check in each
configuration. The legacy Debug and Release configurations each passed 161/161. All 72 C++
headers passed standalone strict C++17 syntax checks; clang-format 19.1.7 checked
182 C/C++ files. The eight seed hashes are unchanged. Both full replay manifests
contain 72 exchanges covering all four operations and the 1,413-holdout test.
The order-8 residual remains exactly `1.0000000560950184e-7`, preserving failure.

See [the source-hashed local verification record](../validation/native-reference-replay-validation-20261013.json).
Windows/macOS results remain pending the actual CI runs for the resulting
revision; no earlier CI outcome is reused as evidence for this change.
