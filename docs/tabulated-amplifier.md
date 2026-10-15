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
