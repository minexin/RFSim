# Native constrained fixed-pump PHD network

`rfmodel/nonlinear/phd_fixed_pump_network.hpp` supplies a dedicated, owning C++17
`PhdFixedPumpNetwork`. It closes one `PhdFrozenSlice` against explicitly prescribed
linear/affine environments, source/load boundaries, and physical port connections.
It supports output matching, harmonic feedback, and nonpump small perturbations.
It solves the frozen first-order affine equations, not the full nonlinear device
under arbitrary loading. Its [coefficient convention](phd-coefficient-contract.md)
is unchanged.

## Problem construction

The constructor takes the frozen slice by value, a complete vector of
`PhdChannelBoundary {channel, source, reflection}`, and exactly one
`PhdPerturbationLimit {channel, maximum_magnitude_sqrt_w}` for every **nonpump**
slice channel. The slice is device 0. Each channel record identifies the original
physical port and harmonic bin; record order is irrelevant. Missing, duplicate,
unknown, and extra records are errors. All boundary quantities must be finite;
DC values must be real. Limits must be finite and nonnegative, including zero.
There is no default unbounded operating domain.

The caller chooses these limits from the validity of their supplied coefficients.
They are an enforced domain declaration, **not a demonstrated physical accuracy
bound**. For each slice channel the solved total incident wave is compared to
`frozen.nominal_incident()`. If any nonpump `abs(a-a0)` exceeds its limit, `solve()`
throws `std::domain_error` and returns no successful result. This check includes
feedback driven by the nonzero F baseline; it is not merely a source-size check.
Environment channels do not have PHD perturbation limits because those maps are
explicitly prescribed affine models, not frozen nonlinear slices.

The pump boundary source must equal the exact stored nominal complex pump;
copy `frozen.nominal_incident()[pump]` rather than independently reconstructing it.
Its reflection must be exactly zero. Every nonzero real or complex reflection,
even subnormal, and every unequal source are rejected before network assembly.
A physical port containing the pump cannot be connected, even if the intended
connection would use only its other harmonics. There is no per-bin wire API.
Nonpump channels on that external port may still have explicit source/load
boundaries. These restrictions define an externally prescribed incident pump;
its outgoing wave remains a solved response and is not clamped.

## Explicit deterministic environments

`add_environment(PhdAffineEnvironment)` takes ownership and returns a device ID
starting at 1. The aggregate contains:

- `model`: a `FrequencyConversionModel` with explicitly supplied fixed D/C
  matrices, channel labels, and per-channel reference impedances.
- `boundaries`: complete labelled `PhdChannelBoundary` records.
- `output_offsets`: complete labelled `PhdChannelOffset {channel, outgoing}`
  records, including explicit zeros. These are total affine outgoing offsets,
  distinct from incident sources.
- `reference_planes`: exactly one `PhdPortPlane {port, reference_plane}` with a
  nonempty identifier for each represented physical port.

The model's frequency step must equal the frozen fundamental **exactly**; no
frequency interpolation, rounding, inferred harmonic mapping, or resampling is
performed. Global size is limited to 512 channels, including the slice. Environment
model matrix order is its own explicit `channels()` order; all other records and
all wiring are aligned by physical port and bin. Arbitrary affine direct/conjugate
maps accepted by `FrequencyConversionModel` are permitted; no nonlinear callback
is invoked. DC remains a real degree of freedom.

`connect(first_device, first_port, second_device, second_port)` makes an ideal
physical wire joining **all** bins on both ports. The complete bin sets must match;
connected source and reflection boundaries must be zero. Duplicate connections
and self connections are rejected. Validation occurs before any connection is
added, so a rejected connection does not leave a partial wire.

Each plane name identifies the caller's original coefficient coordinate. Connecting
two differently named planes explicitly declares that these supplied planes are
joined by an ideal zero-length wire. It does not assert the strings are equal,
translate a plane, de-embed, introduce a phase delay, or insert propagation. Model
any actual connecting line as an explicit environment device. Mixed positive-real
PHD references and valid environment power-wave references (including complex
positive-frequency references) retain their own coordinates. The existing
`solve_waves` V/I junction rules handle the wire; the PHD coefficients are never
renormalized. DC references must remain positive real.

## Equations, result and ownership

The internal assembly uses the frozen D/C and `output_offset()` exactly once:

    b = F_frozen + D_frozen a + C_frozen conj(a)
    a_external = source + reflection * b

The zero pump columns make the frozen offset equal the total F baseline. Neither
phase nor amplitude is re-estimated from solved waves, and F/r is never used as a
derivative. Environments contribute their own supplied total offsets once. Wires
use the existing mixed-reference power-wave boundary equations. The private
assembled map calls only `FrequencyConversionModel::solve_waves`; it does not
construct noise placeholders or invoke noise propagation.

The pump is therefore truly clamped in the equations before solving: it is an
unconnected zero-reflection source with the prescribed exact nominal value.
An exact post-solve equality check is an additional invariant check, not the
mechanism enforcing the clamp. Failure throws; the pump is never overwritten
post hoc to conceal an unconstrained solve.

`PhdFixedPumpResult` owns:

- `channels`: per-device original `{port,bin}` labels, preserved `reference_ohms`
  and `reference_plane`, and total incident/outgoing waves. Device blocks are in
  insertion order, each in its original model channel order.
- `phd_perturbations`: solved `a-a0` in the frozen slice's original channel order.
- `relative_residual`: the deterministic solver's equation residual.
- `prescribed_pump`, `solved_pump`, and `pump_clamp_exact`: explicit exact-clamp
  evidence; every successful result has `pump_clamp_exact == true`.

Inputs, copied/moved networks, and returned results own their data. Mutating or
destroying caller storage cannot alter a constructed problem. Invalid inputs,
singular/overflowing systems, and domain violations throw instead of returning a
successful result. No mutable internal representation, unrestricted
`ConversionDevice`, full conversion model, or Newton adapter is exposed.

## Evidence and exclusions

Focused tests exercise independent real 2x2 and two-feedback-harmonic oracles,
complex conjugate terms and nonzero F, mixed-reference V/I continuity through
multiple environments, fixed-pump cubic Fourier small-perturbation convergence,
phase/DC/ordering/ownership, exact pump rejection, singularity, domain rejection,
and installed two-translation-unit use.

No noise input or output exists. The absence of noise quantities does not declare
the slice noiseless. This interface adds no vendor PHD format, coefficient
extraction, amplitude/frequency interpolation, varying-pump derivative, new Newton
solver, arbitrary-load nonlinear accuracy claim, or SystemVue compatibility
claim. One slice and one frozen operating amplitude remain the entire PHD domain.
