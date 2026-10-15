# DC and small-signal comparison readiness, 2026-10-15

The native analytic cases pass. Commit `dddc0593961b3d734ee7702d96ee9d4ca370519a`
passed all 18 CI jobs ([exact-commit evidence](../validation/dc-phd-ci-dddc059-20261015.json)).
The later [live follow-up](dc-bias-systemvue-followup-20261015.md) constructed
dedicated workspaces and submitted eight analyses. Nonlinear model creation failed;
independent RC voltage spectra were exported but did not pass the original numeric
criterion. **No vendor DC, V/I/Y or S comparison is accepted.**

The earlier [readiness evidence](../validation/dc-bias-systemvue-readiness-20261015.json)
remains the immutable pre-acquisition snapshot: its zero-submission counts describe
that earlier time. The circuit and acceptance contract has not changed.

## Native results and finite source impedance

The current `dc_operating_point_test` passes 316 checks in Windows Debug and
Release. The divider is Vs=3 V, Rs=1 kohm and RL=2 kohm. Its Norton form retains
1 mS source conductance and injects 3 mA: V=2 V, load current=1 mA, total G=1.5 mS.
Ground is node 0; terminal current is positive into a device and injection is
positive into a node. The physical source delivers `(Vs-V)/Rs`.

The cubic case uses Vs=2 V through 1 kohm, device current
`I(v)=0.001*v+0.002*v^3 A`, and charge `Q(v)=1e-9*v+3e-9*v^3 C`.
It solves `v+v^3=1`, with the source conductance included once:

| Quantity | Native result |
| --- | ---: |
| Bias voltage | 0.68232780382834712 V |
| Device current | 0.0013176721961732241 A |
| Total dI/dV, including Rs | 0.004793427391263292 S |
| dQ/dV at bias | 5.1901410868949378e-9 F |
| Charge at bias | 1.6353443923456627e-9 C |

The existing bisection, derivative, residual and ownership checks retain their
original tolerances. Capacitance is dQ/dV, not Q/V. The charge does not alter the
static KCL equation; it supplies the frozen frequency response.

The two-node RC fixture retains these complete matrices:

```text
G = [ .003  -.002 ; -.002  .005 ] S
C = [ 5e-9 -4e-9 ; -4e-9 6e-9 ] F
```

Node 1 has the 1 kohm source resistance and a 1 nF shunt capacitor. The 1–2
branch is 500 ohm in parallel with 4 nF. Node 2 has 1/0.003 ohm in parallel with
2 nF to ground. The new focused checks drive a 1 V RMS source through that finite
1 kohm resistance at 0, 1 kHz and 1 MHz. The native wave boundary supplies only
the 1 mA Norton increment; it adds no second resistor or matched load. The
50/75-ohm references are wave coordinates. Independent 2-by-2 determinant
solutions check both node voltages, source/port currents, KCL and reduced Y.

At 1 MHz the results are:

```text
V1 = 0.011442668024723194 - j0.066010801370684533 V RMS
V2 = 0.009869994716084454 - j0.043305204600424668 V RMS
Is = 0.00098855733197527685 + j0.00006601080137068453 A RMS
Y1 = 0.0025494092775047916 + j0.01470710756148325 S
```

The augmented `frozen_small_signal_test` passes 90 checks in both MSVC
configurations and 91 under local Ubuntu/GCC with
`-std=c++17 -Wall -Wextra -Wpedantic -Werror`. The one-check difference is an
existing extreme-range branch permitted by wider arithmetic; all new finite-source
checks run on both platforms. Original physical V/I reconstruction residual is
2.861e-17 A, and the power residual is 1.111e-16 W. These are native results.

## What the installed SystemVue 2023 help establishes

The local `Help/systemvue.qch`, accompanying SystemVue 2023.0.0.11903, was read
without opening a new workspace. The evidence records the QCH hash and each
decompressed resource hash. Relevant resource names are:

| Help resource | Consequence for the comparison |
| --- | --- |
| `rfdesign/RES.html`, `CAP.html`, `VDC.html`, `IDC.html` | Resistor, capacitor and DC source models exist; their displayed units include ohm, pF, V and A. SI value and unit ID must be read back. |
| `sim/Linear_Analysis.html` | Nonlinear parts trigger internal DC bias before linearization. This documents an analysis capability, not an arbitrary user I/Q callback. |
| `sim/Linear_Analysis_Output.html` | Listed outputs are F, S, ZPORT and optional CS; `stoy` is postprocessing. It does not establish independent internal-node V/I export. |
| `sim/Linear_Analysis_Properties.html` | DC-analysis selection and Gmin options exist. Native RFModel has no implicit Gmin; these settings must be captured. |
| `sim/Spectrasys_DC_Voltages.html`, `Spectrasys_DC_Currents.html` | DC_V/DC_I collect user-entered supply-budget values. They are not solved node voltages/currents. |
| `rfdesign/IPROBE.html`, `Current_Probe_Part.html` | A current probe exists and lists an IDC parameter in A. The inspected help does not specify an independent dataset export or its current direction. |
| `sim/Spectrasys_Voltage_DC.html`, `DC_Baseband_and_Zero_IF.html` | VDC is a separate spectral path measurement; DC spectra are coherent and use peak values. This is not evidence that the requested nonlinear bias circuit was solved. |
| `rfdesign/Nonlinear_Category.html` | The documented RF nonlinear category links Circuit Link and X-parameters. It does not establish an arbitrary I(v)/Q(v) device entry. |
| `rfdesign/Circuit_Link.html` | Circuit Link uses harmonic balance and can automatically insert port matching/isolators. Those alter boundary conditions and cannot be left implicit. |

At the initial readiness inspection, the live session was inspected read-only and
remained the restored
`session-blank-before` workspace, idle and with no manager errors. The installed
RF examples and help did not establish a supported arbitrary cubic I(v)/Q(v)
model entry or an independent internal V/I export for these fixtures. This is a
specific unresolved integration condition, not a claim that the entire product
lacks those capabilities. The subsequent live follow-up found the decisive
installed `sim/Non-Linear_Co-Simulation.html` restriction: SystemVue nonlinear
circuit co-simulation supports X-parameter models, while general nonlinear circuit
simulation is directed to Genesys. NLRES/NLCAP registry entries and successful
parameter readback did not produce usable simulation models in the tested path.

## Required order for the vendor acquisition

1. Establish the supported model/input path and independent node/terminal V/I
   export. Build a dedicated workspace only after those paths are known. Preserve
   the current session and every original project; use explicit ground and finite Rs.
2. Read back source amplitudes, peak/RMS convention, current directions, physical
   node map, all component units, DC/Gmin settings and port references. The source
   resistor remains exactly once in the nodal model. A zero AC increment of the DC
   source does not remove its resistance from G.
3. Acquire and compare physical V/I and Y against the recorded analytic cases,
   using the existing native criteria. A Y or V/I reconstructed only from S must
   be labelled as such and cannot replace independent physical observations.
4. Compare S only after the physical comparisons and wave-coordinate mapping pass.
   Retain all raw observations, warning/error output, parameter readbacks and hashes.

There is no newly assumed ideal-voltage constraint, SPICE/MNA interface, transient
solver or automatic noise law. Existing published nodal-current-noise code remains
present in the current tree; the older cloud snapshot did not describe the later
local commits. Neither that extension nor these analytic cases establish vendor
DC/noise compatibility.
