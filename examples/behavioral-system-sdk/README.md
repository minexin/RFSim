# One behavioral definition, external S and loaded internal ports

Copy this directory, install RFModel, then build against the installed package:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/relocated-sdk
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This two-translation-unit C++17 example needs no Python runtime. It uses only
existing typed APIs, without a new solver, system framework, JSON dispatcher,
MNA/SPICE dependency, C ABI, or UI. `Fixture` is example-local data: immutable
named `ModelAdapter` instances, **one connection table**, and selected external
ports. `build` and `request` consume that same data. Each analysis samples owning
matrices, rather than sharing a mutable solver instance.

## What it demonstrates

- `StaticSParameterModel` represents behavior at physical device ports. Its
  adapter owns the provider and exposes the callbacks it actually implements.
  These examples require stationary linear S; `capabilities().linear` is checked.
  They do not reveal transistor/internal-element nodes or automatically linearize
  every nonlinear device. Unsupported capabilities remain errors.
- `analyze_linear` builds the same topology at 1 and 2 GHz and reports every
  external S entry. Other external ports have their reference terminations for
  each S excitation; this is distinct from the loaded experiment below.
- `analyze_conversion_simulation` uses the same models/wires at one positive
  frequency bin per experiment. A 50-ohm source supplies 1 mW available power.
  Every `(device name, local port, frequency, bin)` receives waves, RMS V/I and
  incident/outgoing/net power, including connected ports inside the graph.
- The chain has two 50-ohm series blocks and 50/100-ohm loads. The branch uses a
  lossless three-port junction plus two series blocks, a `100-j20` ohm main load,
  and `50` or `100+j30` ohm side load. Unequal complex reference impedances on
  opposite sides of each internal wire are coordinates, not physical loads.
  Both branches remain in the solve.

The independent oracles are elementary voltage division and parallel-load KCL,
not another production solver. At 50-ohm external reference the chain S entries
are all 1/2. For the tee with two 50-ohm series branches the external S rows are
`[0,1/2,1/2]`, `[1/2,1/4,1/4]`, `[1/2,1/4,1/4]`. These follow by terminating all
unexcited external ports in 50 ohm and exciting one port at a time. Loaded branch
checks use `Zp = ((50+Zm)^-1 + (50+Zs)^-1)^-1`, `Iin = Vs/(50+Zp)`,
`Vj = Iin*Zp`, with `Vs = 2*sqrt(50*0.001)` RMS. Coordinate-invariance checks
repeat the physical experiment with all-real internal references. Assertions are
throwing checks, still active under `NDEBUG`; invalid wiring/retry, zero-current
ratios and later parameter/source changes are checked too.

## Measurement contract and limits

`a` and `b` are incident and outgoing Kurokawa power waves in sqrt(W). Per-port
reference impedance is explicitly labeled `reference_ohm`, never measured node
impedance. Complex-reference coordinates use
`V=(conj(Zref)*a+Zref*b)/sqrt(Re(Zref))`, `I=(a-b)/sqrt(Re(Zref))`.
The current points **into the named device port**. These are RMS phasors, not
peak amplitudes. `|a|^2`, `|b|^2` are incident/outgoing powers in W;
`|a|^2-|b|^2 = Re(V*conj(I))` is net power into that device. A resistor's output
port therefore has negative absorbed power when delivering power to a load.

`loaded_apparent_V_over_I_ohm` is only the ratio under this actual drive and
current direction; it can be negative at an output port. The example does not report this ratio when
`|I| <= 1e-15 A`: this is only a display-protection floor, not a claim that
a nonzero-current ratio is mathematically undefined. Exactly zero current makes
V/I undefined. Raw RMS V and I remain visible in every case. The ratio is not an
incremental looking-in impedance or SystemVue ZPORT. Each reported bin is a discrete deterministic spectral component, not a
PSD or an integrated noise measurement. Intrinsic noise is deliberately omitted
using the required explicit policy; this does not assert that a resistor is
noiseless, nor implement a noise budget.

The existing `analyze_linear_path` helper is demonstrated only on the ordered
real-reference two-port chain. Its named ratio is load-delivered power divided
by source-available power, with the stated source/load reflections. That helper
does not select arbitrary graph paths.

For the branch, `main_delivered_over_tee_input_accepted` is explicitly a
**selected-cut power ratio of the full loaded graph**: numerator is net power
out of `main/p1` toward its load, denominator is net power into `tee/p0` from
the source. Direction, frequency, source and both loads are printed on every
line. It includes side-branch loading and bilateral reflections. It is not an
isolated-chain gain, unique path attribution, general graph path/noise budget,
or a vendor-equivalent path measurement. No output claims SystemVue CP/CNP/CV
or other measurement-name compatibility.

## Regression integration

`behavioral_system_example` is the focused in-tree target/test.
`behavioral_system_sdk_installed` independently installs full and pure-header
SDKs, moves each install prefix (including spaces), copies this directory, and
compiles/runs its two TUs using only the relocated package. Run that installed
check in both Debug and Release to cover four producer/consumer combinations.
The example is new workflow coverage; the existing frozen-adapter example
instead demonstrates a DC operating-point freeze and subsequent analyses.
