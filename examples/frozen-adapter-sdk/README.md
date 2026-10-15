# Frozen DC → owning ModelAdapter → C++ SDK workflows

This dual-translation-unit C++17 example assembles existing public APIs. It adds
no public factory, numerical algorithm, vendor model interpretation, MNA solver,
noise law, nonlinear rebias, or SystemVue compatibility claim. All oracle checks
throw on failure and remain active in Release/NDEBUG.

## Installed-only build

From the repository root:

```sh
cmake -S . -B build-sdk -DBUILD_TESTING=OFF \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/absolute/path/sdk
cmake --install build-sdk
cmake -S examples/frozen-adapter-sdk -B build-frozen \
  -DCMAKE_PREFIX_PATH=/absolute/path/sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build-frozen --config Release
ctest --test-dir build-frozen -C Release --output-on-failure
```

The example can be copied outside the repository. Its CMake uses only
`find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)` and `RFModel::rfmodel`, with
strict compiler warnings and no source include fallback. Use a complete SDK
installation containing these APIs; the package version alone does not identify
which interfaces an older installation contains. No Python runtime is needed.
The executable prints one result line, writes no files, and has **one CTest**.

The top-level `frozen_adapter_sdk_installed` CTest installs a pure C++ header SDK,
relocates the prefix, copies the example source, builds both translation units,
and executes the example CTest. Together with `pll_phase_jitter_sdk_installed`,
these form the native SDK integration-example verification stage:

```sh
cmake -S . -B build-focused -DBUILD_TESTING=ON -DRFMODEL_BUILD_C_API=OFF \
  -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
ctest --test-dir build-focused -C Release \
  -R '^(frozen_adapter|pll_phase_jitter)_sdk_installed$' --output-on-failure
```

These are affected integration checks, not a replacement for the full numerical
regression suite. The PLL example retains its three consumer CTests.

## Ownership and physical assumptions

`make_model` builds a two-node grounded RC device with
G = [[0.03, -0.01], [-0.01, 0.02]] S, C = diag(1, 2) nF, and current injection
[0.02, 0] A. `solve_dc` gives [0.8, 0.4] V; `freeze_small_signal` retains only the
accepted numeric operating point and Jacobians. The callback's weak lifetime
token expires when the factory returns. Later frequency evaluations must not
increment its evaluation count.

`ModelDescriptor` declares dense physical ports 0 and 1 corresponding to nodes
1 and 2. Both references are explicitly 50 ohms. `ModelFunctions` captures the
frozen numeric state, node selection, references, and optional nodal Q **by value**.
No source device, local vector, or DC callback is borrowed. The conversion
snapshot is evaluated after its temporary adapter dies, and the coherent plan
after its graph dies. Larger hosts may share immutable owned state to avoid
copies without changing these lifetime requirements.

The supplied noise case explicitly declares Q = diag(1e-20, 2e-20) A²/Hz. This
is an example input, not automatic 4kTG or inferred transistor noise. Missing Q
leaves the callback absent: direct noise access and `require_model_noise` both
reject it. Explicit zero Q advertises an available noise model and succeeds.
`omit_intrinsic_noise` is an intentional modeling omission, not a claim that a
physical device has zero noise.

## Independent checks and composition

At DC, 1 MHz and 10 MHz the independent oracle calculates the 2×2 determinant
inverse B = (G + jωC + I/50)⁻¹, S = 2B/50 − I, and covariance B Q Bᴴ / 50.
Every complex S and noise element is checked against both direct model calls
and `analyze_linear`. A one-frequency `StaticSParameterModel` would incorrectly
remove this RC frequency dependence and is deliberately not used.

- Linear/noise: an owning `NetworkBuilder` adds one adapter block at each
  requested frequency. Its intrinsic covariance callback is valid because this
  example has exactly one block. A multiblock host must assemble the full network
  covariance, including any explicitly modeled interblock correlations.
- Conversion: bins {0, 1, 10}, spacing 1 MHz, frequency-major port ordering. All
  6×6 direct, conjugate, intrinsic C/P entries are checked. The stationary model
  has no off-frequency coupling; positive-frequency P is zero and real DC P=C.
  `FrequencyConversionNetwork::add_power_waves` preserves declared references;
  the network signal and full output C/P are also checked.
- Spectrum: `transmit_linear_spectrum` uses the existing common-real-reference
  two-port 0→1 helper. Both complex tone amplitudes and spacing are checked.
- Coherent: `CoherentSystem::linear_network` uses the same owning builder;
  a compiled plan preserves the model and evaluates the expected output power.

These host composition helpers do not require the adapter to claim direct
`spectrum` or `coherent` capabilities. Those flags remain false, as does
`operating_point`: a DC frozen bias is not the nonlinear incident-wave response
and Jacobian contract. No relinearization is performed during these workflows.

The common 50-ohm real coordinates match `LinearNetwork(50)` and
`CoherentSystem(...,50,...)`. Unequal or complex references cannot simply be
relabeled for spectrum/coherent helpers; use a supported explicit coordinate
transformation. Descriptor references must always equal those used to evaluate
S and noise. No implicit conversion is claimed here.
