# Native tabulated fundamental amplifier

This C++17 model supplies an explicit, derivative-bearing radial AM/AM and AM/PM
contract. It is **not** an implementation or compatibility claim for SystemVue
DataPoints, its power-axis choices, zero-dBm behavior, interpolation, or derivatives.
Existing vendor diagnostic and failed cases retain their previous status.

## Data and units

Include `rfmodel/nonlinear/tabulated_amplifier.hpp`. Construct a
`rfmodel::TabulatedAmplifier` from a vector of `TabulatedAmplifierNode` values:

```cpp
rfmodel::TabulatedAmplifier amplifier({
    // radius sqrt(W), output amplitude sqrt(W), unwrapped phase rad,
    // d(amplitude)/d(radius), d(phase)/d(radius)
    {0.0, 0.0, 0.0, 2.0, 0.0},
    {1.0, 1.8, 0.4, 1.4, 0.8},
    {2.0, 2.4, 1.6, -0.4, 1.6}
});
```

The independent coordinate is incident power-wave radius in sqrt(W), not dBm,
watts, voltage, gain, or output power. Each segment is cubic Hermite interpolation
of amplitude and unwrapped phase, using the supplied endpoint slopes. Slopes are
part of the data; they are not estimated, clipped, or inferred. Phase is not
wrapped at pi, so a crossing remains continuous. The resulting curves are C1 at
interior nodes; second derivatives need not agree.

The table must contain at least two finite nodes with strictly increasing
radii starting at zero. The first amplitude must be zero and its amplitude
slope nonnegative. Amplitude must remain nonnegative throughout each
segment, including interior stationary points: nonnegative samples alone do not
suffice. Nonnegativity uses a conservative certificate with bounded dyadic subdivision.
Ordinary internal double roots that touch zero are supported; numerically
ill-conditioned cases may be rejected rather than admitted without a certificate.
Negative amplitude slopes are allowed when the curve remains nonnegative.
Query radii must be finite and nonnegative and lie within the closed tabulated
interval. There is no extrapolation or clipping. Nonfinite inputs and invalid nodes throw
`std::invalid_argument`; finite radii beyond the last node throw
`std::out_of_range`. A result or intermediate coefficient that cannot be represented
finitely throws `std::overflow_error` (possibly at query time).

`response(radius)` returns interpolated amplitude, phase, their radius derivatives,
complex gain, and the radial gain difference needed for linearization. With
A(r), phi(r), g(r) = A(r) exp(i phi(r))/r, that difference is
exp(i phi(r)) [A'(r) - A(r)/r + i A(r) phi'(r)]. At a valid zero endpoint,
g(0) = A'(0) exp(i phi(0)) and the radial gain difference is zero.

`transmit(incident)` evaluates at the incident magnitude.
`transmit(incident, total_radius)` applies the common-radius gain to that incident
wave. The latter supports multiple tones sharing compression.

Numerical evaluation requires round-to-nearest (`FE_TONEAREST`) and ordinary
IEEE floating-point semantics; do not compile with fast-math. A wider `long double`
is not assumed as a portability guarantee. The admitted numerical domain is
conservative: unresolved certification, nonfinite results, and unrepresentable
Hermite coefficient products are reported as exceptions. Results below the double
subnormal range may round to zero under ordinary IEEE semantics; the API does not
promise to preserve every mathematically nonzero result. Interpolation and phase
rotation have ordinary finite IEEE precision, especially for very large unwrapped
phases; arbitrary-precision tiny trigonometric components are not promised.

Hermite evaluation, complex transmission, and cross-tone coupling use scaled
intermediates to preserve representable final results across very different
magnitudes. A genuinely unrepresentable tiny final phase or coupling may round
to zero; this differs from losing a representable result in an intermediate
product or ratio.

## Conversion and nonlinear operating points

Include `rfmodel/conversion/tabulated_amplifier_linearization.hpp`:

```cpp
const std::vector<rfmodel::ConversionChannel> channels{{0, 1}, {1, 1}};
const auto point = rfmodel::linearize_tabulated_amplifier(
    1e6, channels, {.2, 0.}, amplifier);
const auto law = rfmodel::make_tabulated_amplifier_device(
    0, 1e6, channels, amplifier);
```

The device is matched and unilateral: input outgoing waves are zero, each output
RF bin receives the corresponding input bin times the same complex gain, and
output incident waves do not drive compression. Input and output RF grids must
match, but channel ordering is arbitrary. The common radius is the Euclidean norm
of all input RF incident waves. DC may appear only as an inactive zero bin;
nonzero DC is rejected. No new harmonic or intermodulation bins are generated.
The final optional parameters are input port, output port, and scalar reference
impedance (defaults 0, 1, and 50 ohms).

The returned direct and conjugate Jacobians include cross-tone common-radius
coupling and the explicit AM/PM derivative. The factory returns an owning
`ConversionNonlinearDevice`: its table and channel ordering survive destruction
or changes of the caller's original objects. It can be passed to the existing
nonlinear operating-point solver and accepted-point noise analysis. A solver
trial outside the table is rejected rather than silently extrapolated; choose
a domain that covers the intended operating point and iteration path.

## Owning model adapter and coherent systems

Include `rfmodel/adapters/builtin_model_adapters.hpp` and call
`adapt_tabulated_amplifier(name, table, reference_ohms=50., mode=sources_only)`.
The returned `ModelAdapter` owns an immutable table and declares `linear`,
`spectrum`, `operating_point`, and `coherent`; it does not declare intrinsic noise.
It uses matched unilateral physical ports 0 to 1 at the supplied common positive
real reference. `rfmodel.tabulated_amplifier`, revision `1`, identifies the native
contract and makes no vendor claim.

The stationary S matrix has `S21=g(0)` at positive RF frequencies and all other
entries zero. It represents the zero-drive derivative, not the compressed gain
at a nonzero operating point. Frequency zero is rejected because the table is an
RF envelope response, not a DC conduction law. For the ordinary spectrum callback,
the common radius is `hypot` accumulated over all input RF bin amplitudes, and each
bin is transmitted with that radius. A zero-valued DC bin is retained; nonzero DC
is rejected. No bins are added. The operating-point hook uses the same owning
table and the existing analytic direct/conjugate Jacobian, including cross-tone
derivatives; output-incident waves do not contribute to drive.

In the coherent system graph, physical coherent reduction occurs before the drive
power is evaluated. Coherent cancellation reduces the drive, while independent
groups contribute separate powers even at the same frequency. The table's common
complex gain is then applied using the existing linear-network pipeline operation.
This preserves physical identities, bandwidths, kinds, and each history contribution,
including zero or cancelling paths. Individual history contributions are not passed
through the table's radius-domain test: a path may exceed the net physical drive.
The table limit applies to the reduced physical radius. Stage statistics record
`input_power_w`, `common_amplitude_gain` (the magnitude), and `common_complex_gain`.

The default coherent mode rejects distortion-kind input. Explicit
`AmplifierInputMode::propagate_distortion` includes all existing RF components in
the common drive and propagates them with the same gain, without generating new
frequencies. This is the native common-radius approximation; it is not a waveform
nonlinearity, automatic harmonic/IMD generator, or SystemVue DataPoints algorithm.
Subsequent polynomial stages can generate new distortion from the propagated waves
and their preserved source histories. Pipeline power, history and numerical-range
budgets still apply; the table's standalone extreme-amplitude domain does not
override those graph budgets. Out-of-domain drive throws without publishing a
partial graph result. Plans own their adapters and survive caller/builder destruction.

```cpp
auto adapter = rfmodel::adapt_tabulated_amplifier("PA", amplifier, 50.);
rfmodel::CoherentSystem system(1e8, 50., {{"rf", "clock"}});
auto source = system.input("in", {{1, rfmodel::SpectrumKind::source, 1., 1, .1}});
adapter.coherent_stage(system, "PA", {source}, {"amplified"});
auto result = system.compile().execute();
```

## Verification boundary

`tabulated_amplifier_test` checks analytic cubic/quadratic and independent
piecewise polynomial values, derivatives, phase continuity through pi, negative
interior amplitude rejection, Cartesian finite differences, reordered channels,
output-drive exclusion, ownership, invalid/DC cases, and a very large finite
radius. A complex feedback problem has an independently selected exact root;
its covariance and complementary covariance are checked against a real 2x2
Cartesian feedback calculation. `tabulated_amplifier_installed` builds relocated
full and pure C++ SDK consumers with two translation units and no source-header
fallback. These are native mathematical/API checks, not vendor validation.

`tabulated_model_adapter_test` independently checks the shared cubic amplitude and
quadratic phase law across S, spectrum, analytic multi-tone Jacobian and coherent
graph entry points. It also checks zero-drive coherent cancellation, same-frequency
independent powers, phase propagation through a subsequent quadratic stage,
distortion mode admission, frozen ownership, reference/domain failures and history
retention. The installed table consumer uses the owning adapter across translation
units and executes its spectrum and coherent stages.
