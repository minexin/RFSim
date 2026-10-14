# Accepted-state intrinsic noise (native C++17)

An explicit `ConversionIntrinsicNoiseProvider` evaluates one device's intrinsic noise at the accepted deterministic operating point. It is independent of `ConversionNonlinearDevice`: a linear device can also have operating-dependent noise. No stationary `ModelAdapter::noise(f)` semantics, C ABI fields, vendor coefficients or SystemVue equivalence are introduced.

## Entry points and ownership

Include `<rfmodel/conversion/conversion_operating_point.hpp>`. Use:

- `solve_conversion_operating_point_with_intrinsic_noise(devices, connections, nonlinear, providers, initial, options, loaded_noise, additional_source_noise, fixed_offset)` for one common real reference.
- `solve_conversion_operating_point_power_waves_with_intrinsic_noise(...)` for explicit per-channel Kurokawa references.

Only the first four arguments are required; remaining defaults match the original solvers. Original solver names, signatures, default arguments and address-taking types remain unchanged. Empty providers retain the static-noise behavior.

`ConversionSimulationRequest::intrinsic_noise_providers` is an appended, default-empty vector. Nonempty providers require `operating_point`, including for linear devices. The simulation result's `accepted_devices` owns the frozen Jacobian and complete intrinsic noise used by measurements and every noise-factor experiment. These snapshots remain usable after callback captures are destroyed.

A provider has a device index and `evaluate(const ConversionAcceptedState&)`. Its context contains borrowed local references:

- `jacobian`: accepted derivative with exact original channel order, frequency spacing and complete reference vector.
- `incident`: accepted local incoming waves.
- `outgoing`: exact deterministic local response plus the user's original fixed output offset, matching returned waves.
- `fixed_output_offset`: that original offset, zero-filled if absent. This is not the affine linearization offset; subtract it from `outgoing` to recover the unshifted device law without reevaluation.

References are valid only during the synchronous callback. Do not retain them. Captures must outlive invocation and be safe for any concurrent solves initiated by the caller. Return an owning `ConversionIntrinsicNoiseSnapshot` containing `spacing_hz`, ordered `channels`, `reference_impedances_ohms` and `noise` (C/P).

```cpp
ConversionIntrinsicNoiseProvider provider{0, [](const ConversionAcceptedState& state) {
    auto noise = state.jacobian.zero_noise();
    // Illustrative RF law only; q0 must be provided by a calibrated physical model.
    const double q0 = 1e-20;
    noise.covariance(0, 0) = q0 * (1. + std::norm(state.incident[0]));
    return ConversionIntrinsicNoiseSnapshot{
        state.jacobian.spacing_hz(), state.jacobian.channels(),
        state.jacobian.reference_impedances_ohms(), std::move(noise)};
}};
```

This one-channel example assumes a positive-frequency channel; real DC noise requires P=C.

## Invocation, validation and failure

All provider registrations are checked before deterministic callbacks: indices must exist, callables must be nonempty, and device indices must be unique. Original source and intrinsic noise are still validated, even when a provider will replace intrinsic noise.

After deterministic convergence, each registered provider is invoked exactly once in ascending device-index order, outside backtracking. Already-converged initial points also invoke once. Solves that fail before deterministic convergence invoke no provider. The deterministic law is not reevaluated after acceptance.

Each snapshot must match the accepted spacing, full ordered `(port, bin)` labels and every complex reference impedance. Noise storage, dimensions, finiteness, Hermitian C, symmetric P, physical augmented positive semidefiniteness and real-DC consistency are validated through the existing model contract. Invalid metadata is never silently reordered or renormalized.

The complete intrinsic C/P block replaces the device's static block, including when the returned block is zero. Source noise, additional source C/P and all unregistered devices remain unchanged. Independent contributions within one device must be summed by its provider. Cross-device intrinsic correlations and intrinsic/source correlations are outside this API.

`loaded_noise=false` still computes outgoing noise, so providers still run. It only omits loaded incident/cross/net noise statistics; it is not a signal-only mode. Measurements and reference-noise experiments use frozen accepted noise without reinvocation, even when an experiment changes deterministic sources for gain/noise analysis.

Provider exceptions and invalid snapshots propagate without a partial result or changes to caller-owned inputs. External callback side effects cannot be rolled back (including earlier provider calls when a later provider fails); providers should be pure. No retries or backtracking are performed for provider failure.

## Numerical coverage

The native tests use independent scalar bisection and real-quadrature equations for nonlinear feedback, proper/improper C/P and real DC, plus physical voltage/current mixed-reference checks and frozen-noise experiments. The installed SDK includes a two-translation-unit C++17 consumer. Numerical tolerances for thermal-scale densities are relative to the local expected density, not a unit-scale absolute tolerance.
