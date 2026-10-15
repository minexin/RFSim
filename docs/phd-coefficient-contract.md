# Native fixed-pump PHD coefficient slice

`rfmodel/nonlinear/phd_coefficient_slice.hpp` provides a bounded, header-only C++17
polyharmonic-distortion coefficient foundation. It accepts explicitly supplied native
coefficients for **one fixed positive pump amplitude**, one fundamental frequency,
and a finite set of port/harmonic channels. It is not a complete nonlinear network
model or a vendor file importer.

## Coordinates and immutable ownership

`PhdCoefficientData` contains the convention tag `native_rms_power_wave_v1`,
`fundamental_hz`, positive real `port_reference_ohms`, nonempty
`port_reference_planes`, `channels`, `pump_channel`, `pump_magnitude_sqrt_w`,
`baseline_outgoing` (F), square `direct` (S) and `conjugate` (T) matrices, and optional
`provenance`. All arrays must be explicitly complete; supplied zero is data, missing
entries are errors. Construction validates and owns these inputs. Freeze results
and copied Jacobians own their data independently of the original slice and caller.
Const views are valid while their owning object is alive.

The wave unit is sqrt(W), using RFModel RMS power waves and the supplied fixed
positive real reference impedance for each physical port. F has units sqrt(W);
S and T are dimensionless. Harmonics are nonnegative integer multiples of the
fundamental. The pump is exactly one selected harmonic-1 channel with positive
finite magnitude; another port's harmonic-1 channel remains a permitted nonpump
perturbation. There are 1–512 unique channels and 1–1024 densely numbered physical
ports, each represented by at least one channel. Frequencies and all coefficients
must be finite. Port-plane labels are caller-defined coordinate identifiers; they
are retained verbatim. This API does not perform de-embedding, reference-plane
translation, or impedance renormalization. Callers must supply all coefficients in
one consistent set of these coordinates.

## Phase freezing and restricted response

For `freeze_pump(theta)`, let P = exp(j theta), output harmonic m, and input
harmonic n. The phase is finite and reduced modulo 2 pi. The fixed nominal input
has only a0[pump] = r P; other nominal incident waves are zero.

- b0[i] = F[i] P^m
- D[i,j] = S[i,j] P^(m-n)
- C[i,j] = T[i,j] P^(m+n)
- b = b0 + D delta_a + C conj(delta_a), with delta_a[pump] exactly zero

`PhdFrozenSlice::evaluate_perturbation` returns the total outgoing waves, while
`perturbation_jacobian().apply` returns only their incremental response. The typed
Jacobian has bounds-checked scalar `direct(row,column)` and
`conjugate(row,column)` accessors. `evaluate_incident` accepts total waves but
rejects any pump coordinate different from the exact stored nominal input.
Use `nominal_incident()` to avoid reconstructing that floating-point coordinate.
Nonfinite waves and overflowing responses are rejected.

`output_offset()` is the total-wave affine offset d = b0 - D a0 - C conj(a0).
The required zero pump columns make d = b0 in this contract. It must not be
confused with an incremental response or added twice.

Freezing a different theta represents a different prescribed pump phase, not a
Newton step. For a global time-origin change phi, rotate every nonpump harmonic-n
input by exp(j n phi) and freeze the pump at theta+phi. Output harmonic m then
rotates by exp(j m phi). This covariance is distinct from a port-plane shift.

## DC gauge

DC is a real degree of freedom, not two complex quadratures. The native canonical
gauge is exact: S = T for every DC input column. For every DC output row, F must
be real and T = conj(S). Thus DC-to-DC S and T must also be real. Imaginary DC
perturbations are rejected. These are native API conventions, not asserted vendor
file conventions. Frozen DC rows preserve conjugate symmetry exactly despite
phase arithmetic roundoff.

## Integration safety boundary

A single amplitude slice does **not** determine derivatives with respect to pump
magnitude. Recomputing phase from a trial pump also adds phase derivatives missing
from the clamped map. In particular, F/r is not a pump derivative. Both pump
columns must be explicitly zero, and every evaluation rejects a nonzero pump
delta. Zero pump-column entries mean an excluded domain direction, not a physical
claim that pump changes leave output unchanged.

The public API deliberately provides no `FrequencyConversionModel`,
`ConversionDevice`, `ConversionLinearization`, or `ModelAdapter::operating_point`
factory. The general network solvers do not themselves carry an enforceable pump
clamp.
Do not reconstruct an unrestricted device from the scalar derivative entries or
use this restricted tangent in a varying-pump Newton solve. The dedicated
[fixed-pump network](phd-fixed-pump-network.md) owns and enforces
that restriction throughout its deterministic solve, with mandatory caller-supplied
perturbation bounds. It does not expose its internal conversion representation
or make the slice usable as a general network or Newton adapter.

Noise data are unavailable. This slice neither declares noiselessness nor
supplies a noise figure. No zero-noise convenience object is exposed.

## Evidence and exclusions

The phase exponents and driven-column exclusion follow Verspecht and Root,
“Polyharmonic Distortion Modeling,” IEEE Microwave Magazine (2006), equations
5–18: https://www.janverspecht.com/pdf/phd_ieeemicrowavemagazine.pdf.
Equations 1–2 there use **voltage pseudowaves**, so their numeric coefficients
cannot be copied into this native RMS power-wave API without an independently
specified coordinate conversion. The API takes already-native coefficients;
it performs no hidden conversion.

Tests include independently evaluated complex fixtures, global phase covariance,
nonpump finite differences, a long-double real cubic Fourier-convolution oracle
for RMS/DC normalization, real-DC and ordinary linear/conjugate reductions, affine
loaded-feedback algebra, ownership, invalid inputs, and an installed two-TU
consumer. This native foundation establishes no SystemVue2023 coefficient-file
or numerical compatibility. It includes no vendor parser, coefficient extraction,
amplitude/frequency interpolation, extrapolation, de-embedding, full pump
Jacobian, nonlinear operating-point callback, or inferred noise model.

The [installed SystemVue example review](systemvue-xparameter-format-evidence-20261015.md)
records actual legacy PHD and version-2 X-parameter layouts, power-wave equations,
reference impedances and unresolved normalization/reference-plane conditions.
It is format evidence only and does not change this native API's compatibility boundary.
