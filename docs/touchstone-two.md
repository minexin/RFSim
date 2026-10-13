# Native Touchstone 2.0 single-ended S import

The C++17 reader supports the bounded single-ended S-parameter subset of the
[IBIS Touchstone 2.0 specification](https://www.ibis.org/touchstone_ver2.0/touchstone_ver2_0.pdf)
(April 24, 2009). It does **not** claim complete Touchstone, Touchstone 2.1,
SystemVue, or other vendor compatibility.

## Supported input

- Existing legacy `.sNp` input, including legacy two-port noise records.
- Explicit `[Version] 2.0` in `.ts` or `.sNp` files, with 1–1024 ports.
- Required option line, `[Number of Ports]`, `[Number of Frequencies]`,
  `[Network Data]`, and `[End]`; exact declared network/noise counts are checked.
- Both mandatory two-port orders, `21_12` and `12_21`. Other full matrices use
  row-major order. The two-port order keyword remains required for triangular input.
- `Full`, `Lower`, and `Upper` matrix formats. Triangular data are expanded by
  **ordinary transpose**, so a complex off-diagonal value is copied, not conjugated.
- RI, MA, and DB input; Hz/kHz/MHz/GHz units; omitted option categories use the
  specification defaults. Option categories can appear in any order; R requires
  its following finite positive value.
- Optional `[Reference]` with exactly one finite positive real resistance per
  port, on one line or continued across lines. These replace the option-line R
  only for S data.
- Explicit two-port `[Number of Noise Frequencies]` and `[Noise Data]`, with a
  strictly positive declared noise count, five fields on each noise line, and
  `[End]`. Noise and S grids may differ. Frequencies are finite, nonnegative and
  strictly increasing within each grid; the first noise frequency must not exceed
  the highest S frequency.
- Comments and blank lines, case-insensitive keywords/options, and wrapped
  network records. One frequency record cannot share a line with the next.

Mixed-mode data, non-S parameters, information blocks, unknown keywords, and
versions other than explicit 2.0 fail with errors. These are bounded importer
restrictions, not statements that those features are invalid in Touchstone.

Additional deliberate strictness: only `.ts` and `.sNp` filenames are accepted;
`.ts` requires explicit 2.0 and a `.sNp` count must match `[Number of Ports]`.
Duplicate option lines are rejected rather than ignored. Duplicate metadata,
extra/missing arguments, nonfinite values, negative magnitudes/references,
unsupported control characters, incomplete records, and non-comment content after
`[End]` are rejected. A dangling `R` is rejected rather than implicitly filled in.

## Reference and ownership contract

`TouchstoneData` retains the original scalar `reference_impedance_ohms` field as
**option-line R**, including for 2.0 input. It must not be interpreted as a common
S reference when `[Reference]` is present.

New trailing fields preserve source compatibility with existing aggregate
initializers:

- `version`: `TouchstoneVersion::Legacy` or `TouchstoneVersion::V2_0`
- `matrix_format`: `TouchstoneMatrixFormat::{Full, Lower, Upper}`
- `two_port_order`: `TouchstoneTwoPortOrder::{Order21_12, Order12_21}`
- `reference_impedances_ohms`: actual per-port S reference resistances

The reader fills the reference vector for legacy and 2.0 files. For existing
hand-built datasets an empty vector still means a common scalar reference.
`effective_reference_impedances(data)` validates and returns actual references as
`std::vector<Complex>`; `common_reference_impedance(data)` returns the actual
common resistance or throws if the references differ. Both validate option-line R.

`TabulatedSParameterModel::port(i)` and `TabulatedNoiseModel::port(i)` expose the
actual port reference. Models own validated copies: editing the file or input
`TouchstoneData` afterward cannot change a constructed model. S interpolation is
Cartesian linear interpolation in those unchanged S coordinates. Reject and Clamp
policies apply to the relevant S and noise grids.

```cpp
#include <rfmodel/tabulated_noise_model.hpp>

const auto data = rfmodel::read_touchstone("amplifier.ts");
const auto refs = rfmodel::effective_reference_impedances(data);
const rfmodel::TabulatedSParameterModel model("measured", data);
const auto scattering = model.s_parameters(1.0e9);
// refs[0] and refs[1] may differ; do not substitute data.reference_impedance_ohms.
```

## Noise is a separate reference contract

IBIS 2.0 pages 24–25 specify that `[Reference]` has no effect on the noise
parameters. GammaOpt always uses option-line R (50 ohms if omitted). Legacy raw
Rn is normalized by option-line R; **2.0 raw Rn is already in ohms**. The reader
stores physical `noise_resistance_ohms` for both versions. Noise format remains
Fmin in dB and GammaOpt magnitude/angle, regardless of the S format.

For each noise knot, `TabulatedNoiseModel::from_data` interpolates S in its actual
per-port basis, changes S to the common option-line-R basis, reconstructs the
intrinsic covariance using the noise parameters, then transforms that covariance
back to the actual per-port S basis. Subsequent covariance interpolation is
Cartesian in that fixed basis. Unsupported/nonphysical noise parameters fail
existing validation; they are not silently forced into a physical range.

The existing C ABI and CSV CLI describe only a common imported reference. They
accept common-reference 2.0 files and honor an explicit common `[Reference]`
override. Unequal-reference files fail before returning a C handle or emitting
CSV; the native C++ API is required. No new C/Python per-port import surface is
introduced here.

## Independent numerical checks

New native tests use active Release assertions and physically scaled tolerances
(approximately 3e-12; covariance checks scale by kT, not by 1 W/Hz). Fixtures
include nonreciprocal two-port orders, complex symmetric triangular matrices,
separate noise/S grids, boundaries and invalid files, immutable model snapshots,
and these independent equations:

- A 25-ohm series resistor at references 31/73 ohms has D = 129,
  S11 = 67/D, S22 = -17/D, and S12 = S21 = 2 sqrt(2263)/D.
  With source impedance 45+3j ohms and load 50 ohms, I = Vs/(120+3j),
  Vin = 75 I, and Vout = 50 I.
- A unilateral amplifier with S21 = 2 at a common 75-ohm reference,
  Fmin = 2 (linear), GammaOpt = 0 and Rn = 37.5 ohms has C/kT = diag(1,4).
  The equivalent legacy record uses raw Rn = 0.5; the 2.0 record uses 37.5.
  In 50-ohm coordinates C/kT is [[0.96,-0.384],[-0.384,3.9936]].
  This also checks the same amplifier imported in unequal 31/73-ohm coordinates.

These checks are independent mathematical regression evidence, not new vendor
measurements. Build/test results are recorded in the [local validation report](touchstone-two-validation-20261013.md).
