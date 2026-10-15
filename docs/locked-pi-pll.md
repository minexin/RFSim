# Native locked PI PLL: independent small-signal phase-noise budget

This C++17-only subset of A08 assumes an already locked operating point, a
continuous-time ideal PI transimpedance, and exactly zero loop delay. It does not
establish acquisition, actual physical lock, PNCP, SystemVue compatibility,
fractional-N spurs, automatic device-noise laws, or full-band integrated jitter.
No delay field or arbitrary filter callback is offered. A positive real N is an
effective feedback ratio, not a fractional-N modulator simulation.

Include `rfmodel/noise/locked_pi_pll.hpp`. `LockedPiPllParameters` explicitly uses
Kpd in A/rad, Kvco in rad/s/V, N dimensionless, R in ohms, C in farads. All are
strictly positive and finite. A Hz/V tuning gain must first be multiplied by
2*pi; a charge-pump current specification is not automatically Kpd. The reference
phase is at the detector input, with no additional reference divider.

## Defining contract

For positive offset f in Hz, s=j*2*pi*f and Z=R+1/(s*C):

```
e = phi_ref - phi_out/N - phi_divider
v = Z*(Kpd*e + i_noise) + v_noise
phi_out = Kvco*v/s + phi_vco
G = Kpd*Z*Kvco/s
D = 1 + G/N
Href = G/D                 Hdivider = -G/D
Hvco = 1/D                 Hcurrent = Z*Kvco/(s*D)
Hvoltage = Kvco/(s*D)
```

Divider noise is injected AFTER division, current noise BEFORE the filter,
tuning-voltage noise AFTER the filter, and VCO phase noise at the oscillator
output INSIDE feedback. Transfers are complex, with the divider feedback sign
retained. Current and voltage transfers are named with rad/A and rad/V units.

`analyze_locked_pi_pll(parameters, offset_hz, noise)` returns these five transfers,
the offset, five independent output PSD contributions and their sum.
`LockedPiPllNoise` accepts explicit one-sided reference/divider/VCO PSD in rad²/Hz,
detector-current PSD in A²/Hz, and tuning-voltage PSD in V²/Hz. Inputs are finite
and nonnegative; zero is valid. Each contribution is |H|² times its source PSD.
All output contributions and the sum are one-sided rad²/Hz, never W/Hz.
Correlated sources and shared multiple PLL outputs are unsupported: replacing
complex H by |H| or a real `phase_gain` would lose cross-output phase information.

## Stability and binary64 domain

Clearing the ideal integrators gives Q(s)=s²+a*s+b with
 a=Kpd*Kvco*R/N>0 and b=Kpd*Kvco/(N*C)>0. Its roots have negative real part:
real roots have negative sum and positive product; complex roots have real part
-a/2. This proves stability of this restricted linear model only, not lock of
any real PLL. No sampled-denominator test is claimed to prove stability.

The implementation uses bounded `frexp`/`scalbn` products and a common scale for
b, omega², and a*omega, without requiring wider `long double`. It rejects an
ill-conditioned requested frequency when

```
|Q(j*omega)| <= 64*epsilon_double*(b + omega² + a*omega).
```

The criterion is evaluated in scaled coordinates. Positive but near-zero damping
can fail at resonance; positivity alone does not guarantee useful numerical
conditioning. Nonzero final transfer components and nonzero PSD contributions
must be finite normal binary64. The implementation rejects nonrepresentable
results and numerator-component dynamic-range loss instead of silently clipping
noise. Exactly zero components (including algebraic cancellation in binary64)
are valid. Very small terms can round away against dominant polynomial terms;
this is floating-point evaluation, not arbitrary precision. Reference/current
imaginary transfers use their cancellation-free omega³ expressions to retain
low-frequency lag. Total PSD overflow
is rejected. Invalid inputs throw `invalid_argument`; numerical range/conditioning
failures throw `overflow_error`. Evaluation is atomic and has no mutable state.

## One independent output to correlated wave sidebands

`locked_pi_pll_sidebands(channels, carrier_channel, carrier_wave, spacing_hz,
points)` is an optional explicit bridge. Each `LockedPiPllSidebandPoint` has
`offset_bin`, `offset_hz`, and `total_phase_rad2_per_hz`. The contract requires
exact binary64 equality `offset_hz == offset_bin*spacing_hz`; supply offsets
computed from that grid, not arbitrary samples rounded to the nearest bin.
Positive offsets strictly increase, both sidebands must exist above DC, and all
existing phase-noise carrier/channel bounds apply even to zero PSD points.

For Sphi>0 the bridge calls the existing `phase_noise_sidebands` with
SSB=10*log10(Sphi/2) dBc/Hz. Zero PSD points are skipped after shape validation,
never represented by -infinity. For carrier power wave A:

```
C_upper,upper = C_lower,lower = |A|²*Sphi/2  [W/Hz]
P_upper,lower = P_lower,upper = -A²*Sphi/2  [W/Hz]
```

The bridge uses the caller's carrier-wave reference. Existing conversion boundary
renormalization remains responsible for changes to complex/mixed references.
Pass its C/P to wave-noise propagation; never send raw phase rad²/Hz to a W/Hz
channel measurement. Densities are not multiplied by bin spacing. Small-angle
validity and the extent of the sampled offset band remain the caller's
responsibility. This bridge models one independent output only.

## Independent checks and provenance

For Kpd=Kvco=N=C=1, R=2 and f=1/(2*pi), Q=(s+1)² and the independent oracle is
Href=1-j/2, Hdivider=-1+j/2, Hvco=j/2, Hcurrent=1-j/2, Hvoltage=1/2.
Tests also check direct node-equation transfer algebra, Href/N+Hvco=1, low/high
limits, noninteger effective N, unit conversion, separate PSD contributions,
zero/invalid inputs, resonance conditioning, extreme intermediate scales, total
overflow, SSB factor two, complex C/P, zero-density validation and a complex
reference boundary round trip. An installed two-translation-unit consumer checks
the canonical and compatibility headers. These tests do not establish vendor
agreement or remote/platform CI results.

Primary-source context: [TI Clock Conditioner Owner's Manual, chapter 3](https://www.ti.com/lit/ug/snaa103/snaa103.pdf)
describes loop gain, feedback and noise injection transfer functions;
[Analog Devices, PLLs for High-Frequency Receivers and Transmitters, Part 2](https://www.analog.com/en/resources/analog-dialogue/articles/pll-for-high-frequency-receivers-and-transmitters-2.html)
discusses closed-loop noise shaping. The specific equations, signs, PI restriction,
stability proof and numeric oracle above are independently derived RFModel
contracts, not a reproduction of either vendor's implementation.
