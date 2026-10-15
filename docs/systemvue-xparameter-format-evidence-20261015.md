# Installed SystemVue PHD and X-parameter evidence, 2026-10-15

Four example files and the installed SystemVue 2023.0.0.11903 help were inspected
locally. This establishes concrete file/schema evidence, not numerical model
compatibility. No X-parameter simulation, coefficient fitting or vendor import
was performed. Full vendor files remain in the installation; the
[evidence record](../validation/systemvue-xparameter-format-evidence-20261015.json)
contains hashes, schema counts, axis samples and five coefficient values per file.
The [read-only inspector](../scripts/reference/inspect-installed-xparameter-evidence.py)
checks every multiline data-group shape in these selected examples.

## Files actually inspected

Paths below are relative to the installed
`Examples/RF Architecture Design/RF Design Kit/X Parameters/` directory.

| File | Observed structure | Conditions and coefficient evidence |
| --- | --- | --- |
| `ZFL_11AD_PHDModel.mdf` | Legacy `PHDModelData`, four blocks, 20 rows each | 0.5/1/1.5/2 GHz; `# z0(real)=50`; one AN, six FB, 36 S and 36 T columns per row. |
| `X-param_BalAmp_ZFL11AD.mdf` | Exactly the same SHA-256 as the preceding file | A second filename is not independent evidence. |
| `Multitone_Volterra_X-device.xnp` | Version 2.0; two ports, one fundamental; five data blocks of 31 rows | 1/25.75/50.5/75.25/100 MHz; 14 FB, 196 S and 196 T columns. Both port references are 50+j0 ohm. |
| `AMP_Xparams.mdf` | Version 2.0; three ports, two fundamentals; five blocks of five rows | 1 and 1.2 GHz, VDC_3=10 V; 24 FB, 576 S, 576 T, one FI and 24 XY columns. Three 50+j0-ohm references. |

The two legacy files share SHA-256
`6e7e8f439e740a221228d9067582ff797a4db9e1f59103f0ea8a0cb42afab052`.
Each legacy row has 79 typed columns. The two version-2 data layouts have 407
and 1202 columns, respectively. Their declared complex fields consume two real
tokens in real/imaginary order, and each data group's physical lines must follow
the header-line grouping. File extension alone does not identify the version:
`AMP_Xparams.mdf` is version 2.0.

## Units and wave coordinates

The installed `rfdesign/XPARAMS.html` describes version-2 GMDIF blocks
`XParamAttributes`, `XParamPortData` and `XParamData`. The attributes block defines
the version, highest port index and number of fundamental frequencies. Per-port
`RefZ0` can be complex. VAR statements apply to the immediately following block;
the first data column is the innermost independent variable. A mixing index such
as `1_m2` means f1-2*f2, not a negative port number.

The embedded `docimages/portdata_eqn.gif` explicitly gives the power waves:

```text
a_p = (V_p + Z_p I_p)       / sqrt(8 Re(Z_p))
b_p = (V_p - conj(Z_p) I_p) / sqrt(8 Re(Z_p))
```

The accompanying text calls V/I amplitude phasors. With V/I converted from peak
amplitudes to RMS by dividing by sqrt(2), these equations have RFModel's RMS
power-wave scale. This identifies a coordinate conversion for the documented
version-2 convention; it does not establish a complete coefficient conversion.
Port current polarity and physical reference planes still need an explicit
measurement/model correspondence.

AN and AM describe incident wave magnitudes in sqrt(W); AP is phase in degrees.
FB is an outgoing power-wave amplitude. S and T represent direct and conjugate
small-signal contributions; in the unnormalized power-wave equations they are
dimensionless. FI/FV describe DC current/voltage. Dimensional analysis gives
XY in A/sqrt(W) and XZ in V/sqrt(W); those dimensions are an inference from the
documented equations, not new unit fields found in the example files.

Two concrete inconsistencies make comment-only parsing unsafe:

- The multitone example's comment prints an AN range of 10e-6 to 3.16228e-3
  alongside -20 to 5 dBm. Its actual AN column is 0.00316228 to 0.0562341.
  Squaring the stored wave amplitudes gives the stated power range. The numeric
  comment range is therefore a power range, not the raw wave-amplitude column.
- README names `AMP_Xparams.mdl` and describes maximum harmonic number 5; the
  installed file is `.mdf`, and its own leading metadata says HB_MaxOrder=3 and
  XParamMaxOrder=3. Both sources are preserved without silently choosing one.

The raw first row of the legacy 0.5 GHz block includes
`FB_2_1 = -0.0751489568+j0.555220186` and
`S_2_1_1_1 = -0.563206932+j4.18902719`, while the corresponding T entry is zero.
Those are file numbers, not already admitted RFModel coefficients. The helper
does not transform them or infer a physical law from them.

## Remaining compatibility requirements

The installed help warns that pre-2.0 PHD files may already contain Volterra
normalization; disabling normalization does not undo such stored data. Therefore
the legacy coefficient units/scaling cannot be assumed identical to the
unnormalized version-2 equations. The exact legacy conversion remains unresolved.

The version-2 files contain port names such as `Undefined` and `Output`; they do
not establish calibration reference-plane locations or de-embedding. The inspected
examples use real 50-ohm references, so they do not test complex-reference mapping.
The native [fixed-pump PHD contract](phd-coefficient-contract.md) requires named
reference planes, complete arrays, one clamped pump and excluded pump columns.
The legacy sample's nonzero driven S column shows why a whole vendor matrix
cannot simply be copied into that API. Multitone, DC-bias, interpolation and
normalization semantics also extend beyond that bounded native contract.

A future compatible subset needs a frozen mapping of version, units, pump phase,
reference impedance/plane, index ordering and normalization, followed by raw
SystemVue predictions at independent conditions. Missing coefficients remain
missing. The native fixed-pump PHD implementation and native tabulated AM-AM/AM-PM
adapter do not establish compatibility with these vendor files or their simulator.
