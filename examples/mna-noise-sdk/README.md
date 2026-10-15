# MNA → ModelAdapter → thermal noise → conversion (C++17)

An installed-only, two-translation-unit host example using existing RFModel APIs.
It builds a genuinely source-free series R/L circuit, extracts its complex-reference
power-wave S, supplies explicitly isothermal noise through an owning ModelAdapter,
and consumes those results in LinearNetwork, loaded_noise and a six-channel
FrequencyConversionNetwork. There is no new solver or Python/C API dependency.
All numerical checks throw exceptions and remain active in Release/NDEBUG.

## Build against an installed SDK

From the RFModel source root, install the pure C++ SDK, then build the example:

```sh
cmake -S . -B build-header-sdk -DBUILD_TESTING=OFF \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_PREFIX=/absolute/path/sdk
cmake --install build-header-sdk
cmake -S examples/mna-noise-sdk -B build-mna-noise \
  -DCMAKE_PREFIX_PATH=/absolute/path/sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build-mna-noise --config Release --parallel 2
ctest --test-dir build-mna-noise -C Release --parallel 1 --output-on-failure
./build-mna-noise/mna_noise
./build-mna-noise/mna_noise --variant
```

The example directory may instead be copied anywhere outside the source tree.
Its CMake uses only `find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)` and
`RFModel::rfmodel`; no source include fallback. Use an SDK containing
`rfmodel/mna/power_wave_s.hpp`, since earlier 0.1.0 installations lack this API.
CMake 3.20+ and C++17 are required. On Windows with a multi-configuration generator,
run `build-mna-noise/Release/mna_noise.exe`. Both TUs compile with strict warnings.
The executable writes only stdout/stderr and never creates output files.

## Smallest host integration

Copy `mna_noise.hpp` and `mna_noise.cpp` into a C++17 host linked to
`RFModel::rfmodel`. The reusable factory returns the existing native C++ interface:

```cpp
const auto model = mna_noise_example::make_model({}); // R=5 ohm, explicit T=290 K
const auto s = model.s_parameters(1e6);
const auto c = model.noise_correlation(1e6); // full 2x2, W/Hz
rfmodel::LinearNetwork network;
const auto first = model.add_to(network, 1e6);
const auto external = network.external_noise({first, first + 1}, c);
const auto snapshot = model.conversion_device(
    1e6, {1, 2, 3}, rfmodel::ModelNoisePolicy::require_model_noise);
```

The single-block snippet's C covers every network port. A multi-block network
needs globally indexed covariance; use `independent_noise({C1, C2})` only when the
blocks really are independent. Preserve each block's off-diagonal correlations.
`run(circuit)` returns an owning `Result` with full per-frequency S/C, loaded C,
physical load V/I and resistor power, cascade S/C, conversion snapshot and analysis.
It also executes this example's physical checks. `main.cpp` just selects a fixture
and prints results. Factory input is copied; finalized MNA network, oriented ports,
references, SolveOptions and the noise callback's scattering callable are captured
by value. Captures are const, with no borrowed references or retained builder.

## Explicit circuit and noise assumptions

Default fixture: series R=5 Ω, L=1/(π·10⁶) H; both ports are referenced to ground,
with local references z₁=4+j3 Ω and z₂=9−j2 Ω. Frequencies are 1, 2 and 3 MHz.
All dissipation is in the one resistor at the explicitly supplied 290 K. The
variant changes R to 11 Ω, L to 1.5 times the default, and T to 307 K, checking that
the complete chain responds to inputs rather than a hard-coded S or covariance.

`passive_thermal_noise(S,T)` is justified here by the known passive composition
and uniform equilibrium temperature. Deactivating independent MNA sources does
not establish physical temperature, device noise, passivity or equilibrium.
A numerical passivity check alone is also insufficient evidence of those facts.
Different resistor temperatures, active/non-equilibrium devices, nonlinearity,
quantum corrections and generic noise inference from an arbitrary MNA circuit
are outside this example. The single-sided classical Johnson density is 4kTR
V²/Hz, with k=1.380649×10⁻²³ J/K. S is dimensionless; wave covariance is W/Hz.
RMS phasors give absorbed power Re(V·conj(I)) without an extra factor of 1/2.

The Kurokawa convention is

- a=(V+zI)/(2√Re z), b=(V−conj(z)I)/(2√Re z)
- I=(a−b)/√Re z, V=(conj(z)a+zb)/√Re z

For this explicitly isothermal passive network, C=kT(I−SSᴴ) is valid in these
complex power-wave coordinates. This must not be applied to arbitrary pseudo-wave
S or to a covariance in different coordinates. The example jointly calls
`renormalize_power_waves` and `renormalize_noise`, then verifies both against a
new MNA extraction and an independent physical oracle at the new references.

`Noise::missing` means no noise capability. `Noise::explicit_zero` explicitly
supplies zero classical intrinsic noise (the T=0 model); it does not assert that
a 290 K resistor is noiseless. Missing noise with `require_model_noise` rejects;
`omit_intrinsic_noise` is a conscious caller policy. The factory accepts T=0,
whereas the demonstration's `run` needs T>0 for its error/kT report.

## Independent physical checks

No expected matrix is calculated by MNA stamping, SDK matrix inversion, or the
production thermal-noise formula. From V₁−V₂=(R+jωL)I and I₁=−I₂=I, define
D=R+jωL+z₁+z₂. Direct elimination gives

- S₁₁=1−2Re(z₁)/D, S₂₂=1−2Re(z₂)/D
- S₁₂=S₂₁=2√(Re(z₁)Re(z₂))/D
- A resistor's independent voltage-noise source gives
  C=4kTR/|D|² · [[Re(z₁), −√(Re(z₁)Re(z₂))],
  [−√(Re(z₁)Re(z₂)), Re(z₂)]]

For the default fixture, D=18+j(1+2f/10⁶) Ω and
C/kT=[[80,−120],[−120,180]]/|D|². The negative cross correlation is essential.
All four entries are checked; checking only output PSD would miss its loss.

Physical left/right boundary impedances are Zs=6+j1 Ω and Zl=12−j4 Ω.
For the DUT boundary a=Γb+e, Γ=(Zload−z)/(Zload+conj(z)); the load's own S is
(Zload−conj(z))/(Zload+z), which is different. The deterministic input is a
Thevenin RMS voltage Vs=0.2+j0.1 V. The example solves the extracted S with
boundary emission e=Vs√Re(z)/(Zs+conj(z)), and compares recovered V/I and R|I|²
against I=Vs/(Zs+Zl+R+jωL).

Loaded noise has a second independent oracle with three uncorrelated Johnson
voltage sources Es, El, Ed for the two boundaries and the DUT resistor:
J=(Es−El−Ed)/(Zs+Zl+R+jωL), V₁=Es−ZsJ, V₂=El+ZlJ, I₁=J, I₂=−J.
Each source's analytic a/b coefficients are multiplied by its 4kT Re(Z) density
and their outer products summed. Both full incident and outgoing covariance are
checked. At the circuit temperature everywhere, net thermal flow into each DUT port is zero but
individual incident/outgoing spectra are nonzero. With zero-temperature
boundaries the warm DUT emits nonzero net power; changing reference coordinates
and recomputing Γ and boundary noise preserves that physical power flow.

Two identical RL blocks are also physically connected with different internal
references (9−j2 Ω against 16+j1 Ω; final output 25−j4 Ω). Their full 4×4
independent block covariance is propagated through LinearNetwork and checked
against the directly combined series 2R/2L circuit. Each resistor's individual
noise contribution is checked independently.

All comparisons use dimensionally appropriate scales: dimensionless S, kT W/Hz
for thermal matrices and near-zero flow, physical voltage/current/power scales
for deterministic quantities. Relative tolerance is 2×10⁻¹¹, with no 1 W/Hz
floor that could hide a missing thermal signal. Roundoff zeros are tolerated
against the relevant physical scale; explicitly zero/omitted noise is checked
exactly. Checks survive Release, and callbacks remain usable after invalid input.

## Six-channel stationary snapshot

`conversion_device(1e6,{1,2,3},require_model_noise)` owns channel order
(p0,1 MHz), (p1,1 MHz), (p0,2 MHz), (p1,2 MHz), (p0,3 MHz), (p1,3 MHz).
Every element of direct S, conjugate response, full 6×6 C/P and all reference
coordinates is checked after the temporary adapter has been destroyed. Each
same-frequency 2×2 C is retained, cross-frequency C is zero, and P is zero for
these strictly positive stationary bins. This is a sampled linear snapshot,
not a mixer or an intrinsic cross-frequency noise model. The example excites
each positive bin and runs the actual FrequencyConversionNetwork analysis,
checking outgoing waves and the complete propagated noise.

MNA S requires strictly positive frequency; no DC bin is admitted here.
The adapter supplies only linear/noise capabilities. It does not claim spectrum,
coherent, nonlinear operating-point or new SystemVue compatibility/measurements.

## Installed regression

Top-level CTest `mna_noise_sdk_installed` builds full and pure SDK installations,
moves each to a path containing spaces, copies this directory, then configures
and runs all three consumer CTests using only the relocated package. The pure
producer and both consumers use C++ only; neither install enables Python.
Existing cross-platform Debug/Release CI picks up this top-level CTest.

```sh
cmake -S . -B build-focused -DBUILD_TESTING=ON \
  -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF \
  -DRFMODEL_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
ctest --test-dir build-focused -C Release -R '^mna_noise_sdk_installed$' \
  --parallel 1 --output-on-failure
```

This focused integration check supplements core numerical tests and exact-commit
CI; it is not evidence of additional vendor measurements or a public SDK release.
