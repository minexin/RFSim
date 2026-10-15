# Native grounded DC bias and frozen small signal

This C++17 slice solves a finite, dense grounded nodal circuit and freezes its
analytic quasi-static derivatives. It is not a SPICE implementation: no ideal
voltage sources, inductors, MNA constraints, DAE, transient analysis, hidden
frequency state, vendor DC compatibility, or automatic noise model is provided.

Include `rfmodel/nonlinear/dc_operating_point.hpp` and
`rfmodel/linear/frozen_small_signal.hpp`. Compatibility flat includes are installed.

## Units, signs, and ownership

`DcNetwork::non_ground_nodes` is 1..1024. Node 0 is exact positive-zero ground;
node n occupies entry n-1 in voltage and injection arrays. A device has 1..1024
terminals mapped to nodes, including ground. Repeated node mappings are legal and
stamp by addition. Current is positive **into the device**, injection positive
**into the node**, so KCL is sum(device currents) minus injection = 0.

The callback receives terminal voltages in volts and returns currents in amperes,
and row-major analytic dI/dV in siemens. Optional paired charge values in coulombs
and dQ/dV in farads supply quasi-static capacitance. All entries, including ground
rows and columns, must be finite with exact dimensions. Missing charge means zero
Q/C. Neither symmetry nor passivity is required; derivative correctness is the
model author's responsibility. All-ground devices are permitted but provide no
unknown-node equations. No derivative is estimated numerically.

Finite source impedance stays in the network: a source Vs through Rs is represented
by a resistor Rs to ground plus constant injection Vs/Rs. The resistor stays in
small-signal G, while the independent DC injection has zero AC increment.

`solve_dc(network, initial_volt, options)` borrows the network synchronously.
Callbacks and optional `in_domain(terminal_volt)` predicates must be deterministic;
reference captures must remain alive and unchanged until the solve returns.
Their thread-safety belongs to the caller, and no callbacks execute in parallel.
An empty predicate is unconstrained. Before each assembly every device's domain
is checked before **any** current/charge callback runs. False initial feasibility
throws `domain_violation`; a false trial predicate requests bounded backtracking.
Unexpected exceptions in predicates or callbacks, including non-standard ones,
are wrapped with nested exception and device/iteration context and are never retried.

`DcOperatingPoint` has no unchecked/default construction and owns only the exact
accepted numeric batch. Const getters expose voltage, residual, assembled G/C,
per-device terminal I/Q/derivatives, update count, completed network-batch count,
and the two convergence norms. Rejected domain-only trials do not count as batches.
`freeze_small_signal(point)` owns a numeric copy, never a network or callable; it
performs no reevaluation. Frequency operations remain valid after the model is
changed or destroyed. Independent const reads on the frozen numeric object use
no mutable global cache.

## Explicit nodal current noise

`external_noise(f, external_nodes, references, NodalCurrentNoise{Q})` propagates
an explicitly supplied stationary current-injection covariance to intrinsic
outgoing power-wave covariance (W/Hz). `Q` is an N-by-N Hermitian positive
semidefinite matrix in A^2/Hz, ordered by non-ground node ID minus one, including
internal nodes and all cross-correlations. The caller defines the spectral-density
convention and supplies the covariance at the accepted bias and requested frequency.
The API never derives noise from G, assumes a temperature, adds termination noise,
or reevaluates DC callbacks. It describes stationary noise; cyclostationary
frequency coupling and complementary RF noise require the conversion APIs.

With zero incident waves at the selected ports it solves
`(Y + diag_external(1/z)) V = j` and forms `b_k = sqrt(Re(z_k))*V_k/z_k`.
Other nodes have zero external current except for their explicit noise injections.
The sign of j is positive into the node. Transposed solves obtain the port transfer
rows without inverting Y; the existing factor-based covariance propagation retains
correlated low-rank cancellation. Ports follow the requested order, not node order.
The returned value owns its covariance. At f=0, covariance and references must be
real; the result is the real DC covariance (C=P when lifted to a conversion model).

This noise solve uses the full reference-loaded nodal system. A singular internal
Yii can therefore be acceptable for noise if the full loaded system is well posed,
even though `external_y`/`external_s` reject that internal Schur complement. A
singular or ill-conditioned loaded boundary fails even for zero supplied noise.
Existing frozen-solver numerical range and backward-error rules apply to the
transfer. The pivot threshold retains the pre-cancellation admittance/load scale,
so an almost cancelled active conductance and reference termination fail explicitly
even when extended precision retains a tiny nonzero residual. Existing noise PSD
admission tolerances apply to Q. No clipping or
implicit uncorrelated approximation is performed.

For a resistor with conductance g at T, the caller may explicitly supply
`Q=4*k*T*g`. A resistor between two non-ground nodes contributes equal diagonal
terms and negative off-diagonal terms; omitting these correlations changes the
circuit's physical noise. `frozen_noise_test` checks RC thermal equilibrium,
internal-node elimination against determinant formulas, complex references,
nonsymmetric active responses, correlated cancellation, malformed/DC inputs,
and callback counts. The installed DC consumer checks an explicit 50-ohm
resistor at 290 K. These are native mathematical checks, not SystemVue validation.

```cpp
// `frozen` is a one-node accepted 50-ohm resistor with no other conductance.
constexpr double kT = 1.380649e-23 * 290.;
const rfmodel::NodalCurrentNoise injection{{1, {4. * kT / 50.}}};
const auto covariance = frozen.external_noise(1e6, {1}, {50.}, injection);
// covariance.watts_per_hz(0, 0) == kT for a matched reference.
```

2026-10-15 verification: the nodal-noise stage passed six focused Windows/MSVC
tests in both Debug and Release, including relocated full/pure SDK consumers.
The initial CI exposed four Ubuntu core/SDK failures on the active-load cancellation
rejection case; those [failed results](../validation/frozen-nodal-noise-failed-ci-38705856824.json)
remain recorded. The pre-cancellation scale correction reproduced baseline failure
and fixed success under the same local Ubuntu/GCC toolchain; both Windows
configurations were rerun ([local evidence](../validation/frozen-nodal-noise-boundary-fix-20261015.json)).
Code commit `a0218741785dba724f30c779dc2596284fae9d66` then passed all 18
Windows/Ubuntu/macOS Debug/Release core, SDK and archived-replay jobs in
[CI 38706950468](https://github.com/minexin/RFSim/actions/runs/38706950468).
[Per-job evidence](../validation/frozen-nodal-noise-ci-38706950468.json) is bound to
that exact code commit; later evidence-only changes are not the tested head.

## Convergence and failures

Separate fixed positive current scales Is (default 1 mA) and voltage scales Vs
(default 1 V) normalize the equations. Current threshold is absI + relI*Is;
voltage threshold is absV + relV*max(Vs, abs(v)). Success requires both the KCL
residual and the **undamped** Newton correction below their respective thresholds.
Every point, including zero residual, must pass real scaled partial-pivot LU and
backward-error checks. The pivot threshold is relative to the scaled Jacobian;
there is no 1 S floor. Singular or numerically ill-conditioned systems fail,
including floating zero-current circuits and a pure v^3 root at zero.

Armijo search uses the fixed-scaled residual norm, strict descent, and bounded
trials. Nonfinite candidate voltages and explicit domain rejections backtrack;
invalid callback arrays or arithmetic range failures abort. There is no gmin,
implicit grounding, pseudoinverse, current clipping, or silent derivative repair.
The final permitted Newton update is checked for convergence. Defaults are 100
accepted updates and 30 backtrack reductions after the full-step trial; upper
bounds are 1,000,000 updates and 10,000 reductions. At least one update must be
allowed. Failure never returns a usable partial/best-so-far operating point.
Configuration errors throw `invalid_argument`; solve errors are `DcSolveError`
with `DcSolveFailure` classifications. Finite representability and backward
stability are guarded, not a guarantee of forward accuracy or global root uniqueness.

Extended `long double` intermediates are opportunistic; correctness does not require
that type to be wider than `double` (it is not on MSVC). Operand-aware multiplication
and division guards reject nonzero terms lost to zero before a later operation can
hide the loss. Both solver paths conservatively reject subnormal product/quotient
intermediates; both paths also reject subnormal narrowed double values. The supported numerical domain requires representable intermediate
arithmetic as well as representable public results. An extreme circuit or reference
combination may therefore return an explicit numeric-range failure even when a
symbolically rearranged formula has a representable answer. Exact success/failure
boundaries can differ by platform precision; silent underflow is not an accepted
successful result. Ordinary fixtures and explicit lost-RHS/mixed-reference
regressions are additionally tested with a separate test copy of both headers using
`double` intermediates; this is width emulation, not a Windows ABI/build claim.

## Frozen frequency API

- `nodal_y(f)` returns G+j*2*pi*f*C, with rows current equations and columns voltages.
- `external_y(f, nodes)` selects distinct non-ground nodes in the requested order,
  with deterministic complementary internal order, and solves the complex Schur
  complement at that frequency. G and C are never reduced separately. Singular
  internal Yii fails even when the full matrix is nonsingular.
- `external_s(f, nodes, references)` uses direct admittance-to-power-wave conversion.
- `linear_network(f, nodes, references)` returns one `LinearNetwork` block. Its
  local ports are 0..P-1, **not** original DC node IDs.
- `operating_point()` exposes the copied numeric provenance.

Frequency must be finite and nonnegative, with representable angular frequency and
output entries. References must have finite real/imaginary parts and strictly
positive real parts. Public `admittance_to_power_wave_s(Y, references)` accepts
singular Y, including zero, and solves the RMS Kurokawa equation
`(I + D Y D^-1 Z) S = I - D Y D^-1 conj(Z)`, D=diag(sqrt(Re Z)).
Reference z_j multiplies column j; nonsymmetric Y and unequal complex references
are supported. Singular/ill-conditioned wave boundary fails. This dimensionless
wave equation uses an identity-scale pivot floor; the siemens-valued Schur solver
does not. Open Y gives identity S without inversion of Y.

Pass a by-value frozen adapter to `analyze_linear` with local external-port indices.
Without an explicit user-supplied noise callback the analysis noise vector remains
empty. In particular nonlinear G does **not** imply a thermal covariance 4kTG.

Focused tests use analytic dividers, scalar bisection, nonproportional RC Schur,
independent determinant-based physical V/I power reconstruction, active singular
boundaries, ownership/callback counts, explicit-domain recovery, and relocated
installed two-translation-unit consumers. They make no new vendor-validation claim.

The [2026-10-15 comparison review](dc-bias-systemvue-verification-20261015.md)
adds explicit finite-source two-node V/I checks, covered by all 18 jobs for
`dddc059`. The [live follow-up](dc-bias-systemvue-followup-20261015.md) preserves
nonlinear model-creation failures and independent RC voltage exports that fail
the original criterion. No vendor DC or small-signal comparison is counted as
passed; native code and tolerances are unchanged.

## Host SDK composition example

See the dual-TU [frozen adapter SDK example](../examples/frozen-adapter-sdk/README.md)
for an owning frozen RC model evaluated after its source callbacks are destroyed.
It uses existing `ModelDescriptor`/`ModelFunctions` callbacks, explicit nodal Q,
and installed SDK workflows for linear/noise/conversion/spectrum/coherent use.
It does not infer a noise law, add a factory, or freeze frequency dependence by
storing one sampled S matrix.
