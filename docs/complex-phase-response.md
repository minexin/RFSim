# Explicit complex phase-response sources (native C++17)

`rfmodel/noise/phase_response.hpp` generates a complete source covariance C and
complementary covariance P from an explicitly supplied shared reference phase
process. This is a finite-offset, small-angle source model, not a PLL solver,
clock-source inference, stability/locking test, or SystemVue/PNCP compatibility claim.
The original real `PhaseNoiseCarrier::phase_gain`, simulation request layout,
and C ABI are unchanged. No C/Python/JSON API is added.

## Contract and units

`PhaseResponseCarrier{channel, wave}` identifies a carrier by index in the declared
channel grid; `wave` is a nonzero finite complex power wave at one explicitly
chosen positive real generation reference. Every
`PhaseResponsePoint{offset_bin, offset_hz, reference_phase_rad2_per_hz, member_response}`
owns the positive-offset sample and its per-member complex transfer responses.
The response vector has exactly the carrier count, in the same order.

With the positive-frequency convention exp(+j 2 pi f t), the dimensionless phase
response is H(+f), and H(-f)=conj(H(+f)). The reference PSD S_phi is one-sided,
in rad²/Hz, not W/Hz and not an output total PSD. The corresponding SSB level is
L=10 log10(S_phi/2) dBc/Hz. Zero PSD is represented directly by numeric zero.

For each positive offset, one proper complex latent variable u has E|u|²=1 and
E[u²]=0. Each carrier contributes:

- U[carrier+offset] += j a H sqrt(S_phi/2)
- V[carrier-offset] += j a conj(H) sqrt(S_phi/2)
- z=Uu+Vconj(u)
- C=UU^H+VV^H; P=UV^T+VU^T

Only H is conjugated in the lower coefficient, not j or a. Coincident sidebands
sum latent coefficients before moments; this can produce nonzero P on the
matrix diagonal. Separate positive offsets are independent. Output C/P is a
W/Hz density, without multiplication by spacing or bandwidth. An isolated
sideband has density |a|²|H|²S_phi/2. A single-output total PSD cannot reconstruct
cross-output phase; neither |H| nor a PLL total output PSD replaces explicit
shared-reference H and S_phi.

For a1=a2=1, H1=1, H2=j, and ell=S_phi/2, C(u1,u2)=-j ell,
C(l1,l2)=+j ell, P(u1,l2)=+j ell, and P(u2,l1)=-j ell.
The normalized difference-sideband density is ell*(1-cos(theta)) for H2=exp(j theta).

## API and normalization

```cpp
#include "rfmodel/noise/phase_response.hpp"
auto noise = rfmodel::phase_response_group(
    channels, {{carrier_index_1, amplitude_1}, {carrier_index_2, amplitude_2}},
    spacing_hz, {{offset_bin, offset_bin * spacing_hz, reference_psd, {H1, H2}}});
```

The low-level generator cannot infer power-wave references. For a simulation
with local complex references use the independent adapter:

```cpp
#include "rfmodel/conversion/conversion_phase_response.hpp"
rfmodel::ConversionPhaseResponseGroup group{
    "explicit reference", {{{device_1, port_1, carrier_bin}},
                           {{device_2, port_2, carrier_bin}}},
    {{offset_bin, offset_bin * spacing_hz, reference_psd, {H1, H2}}}};
auto prepared = rfmodel::with_phase_response_noise(request, {group}, 50.);
auto result = rfmodel::analyze_conversion_simulation(std::move(prepared));
```

The adapter resolves physical device/port/bin endpoints, validates topology and
external boundaries, transforms source and reflection into the chosen real
generation reference, generates full C/P there, and transforms each sideband
back to its local reference. It adds the result to existing
`additional_source_noise`, independently validating that process before addition.
Both C and P are transformed. Internal connected ports cannot carry generated
boundary sources. No device transfer renormalization or network solve is needed
for preparation; normal simulation validation and solve remain in the existing
analyzer.

Different group objects represent statistically independent processes. Only
members within one group share a latent process. Names must be unique within
one helper call and are diagnostics, never evidence of a physical common clock.
Repeated helper calls add further independent noise; they do not replace or
deduplicate prior contributions by name. Existing real-gain phase groups remain
independent and are handled by the original analyzer. Shared-plus-independent
residual decomposition must be supplied explicitly by the caller.

Requests, groups, response tables and returned matrices own their values. The
helper copies its input request and returns a new owning snapshot; failure does
not partially modify the caller's request. It retains no callbacks or borrowed
memory. Reordering channel arrays is safe when endpoints and carrier indices
are updated to their physical identities.

A common generation reference defines electrical wave coordinates, not clock
correlation. Changing that reference and regenerating j*a noise can define a
different physical source, particularly with frequency-dependent source
impedance. Coordinate equivalence means transforming an already fixed source
model, its reflection, and complete C/P together.

## Validation and limits

Spacing must be finite and positive. Positive integer offsets must strictly
increase. `offset_hz` must equal `offset_bin * spacing_hz` exactly, with a finite,
positive product; no nearest-bin rounding, interpolation, or extrapolation is
performed. Carrier bins and both sidebands must be strictly above DC, explicitly
present on the same port, and representable without integer overflow. This first
version rejects DC/cross-DC/negative absolute-frequency sidebands.

Channel identities and carrier members cannot be duplicated; group names must
be unique within a call. Response dimensions,
finite H/PSD, and grid structure are checked even when H or PSD is zero. Negative
PSD, NaN and infinity are rejected. The bounded grid limits match the existing
noise generator: at most 512 channels, 255 offsets, and 512 high-level groups.

Amplitude scaling uses bounded double mantissas and base-two exponents, avoiding
premature |a|²|H|² overflow or underflow and avoiding dependence on a wider
`long double`. Each nonzero member sideband density must be finite and normal;
unrepresentable coefficients, loss of a nonzero complex component during
scaling, unrepresentable nonzero component products, and accumulation/product
overflow throw. Exact zero sources and exact
coherent cancellation are permitted. This is not arbitrary-precision support.
This conservative range contract does not promise to accept every mathematically
representable final result. The original real-gain generator's numerical boundary
is not changed.

This finite sample model omits carrier attenuation, higher-order sidebands,
continuous realizability and unsampled offsets. A Hermitian sampled response
alone does not prove a causal/stable physical system.
