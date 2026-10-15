# RFAMP OP1dB local pilot, 2026-10-15

This stage follows the unchanged [four-run acquisition plan](../validation/rfamp-op1db-pilot-plan.json).
A and B have been acquired from the dedicated `RFModel_CascadeIntermods.wsv`
copy with SystemVue 2023.0.0.11903. Both acquisitions are warning-free, and their
actual complex input, complete local raw directories, parameter readbacks and
unit metadata are retained. No original vendor project was overwritten.

## A/B evidence and prediction freeze

A uses first-stage OP1dB=20 dBm; B changes only that parameter to 21 dBm.
Their submitted scripts differ in exactly that setting. Across the 124 parameter
entries, only the corresponding expression and SI value differ: 0.1 W versus
0.12589254117941676 W, with unchanged original unit ID 11002. The source remains
one 1 GHz tone at 5 dBm and 0 degrees, and the second-stage OP1dB remains 20 dBm.

Both measured input waves are `0.056234131816108215 + j0 sqrt(W)` at the fixed
50-ohm reference. Their squared magnitude is `0.0031622775811114344 W`.
Delivered P1/RFPwrIn is separately retained as `0.003162277596922826 W`; neither
it nor nominal 5 dBm replaces the measured voltage normalization.

The existing native common limiter gives input scale 1 for both single-tone
identification cases. Their fundamental-wave relative residual is approximately
8.9e-16. The isolated odd harmonic coefficients, in the existing voltage-domain
polynomial convention, are:

| Order | A coefficient | B coefficient | B/A |
| --- | ---: | ---: | ---: |
| 5 | -0.07786106651126515 | -0.1971254375477264 | 2.531758764429534 |
| 7 | -0.025165408373618546 | -0.08997270771950767 | 3.575253235859588 |
| 9 | -0.003912668687190634 | -0.015325185082200548 | 3.9168113396292457 |
| 11 | -0.00020418919449809446 | -0.0008619208142545304 | 4.221187200298075 |

Using A coefficients with the B common response fails all 18 selected B origin
checks at the unchanged 1e-7 relative complex-wave tolerance. These are controls
and identification observations, not independent holdout points. This evidence
rejects the tested unchanged-coefficient hypothesis at this profile; it does not
identify the proprietary coefficient law.

The [analysis tool](../scripts/reference/analyze-rfamp-op1db-pilot.py) identifies
coefficients only from A/B and freezes the B coefficient array, normalization,
source hashes and C predictor before C acquisition. The native C++ implementation
performs the common-response and polynomial calculations; Python is a developer
evidence tool, not a product runtime interface.

There are 380 predicted C origins: 28/60/110/182 at orders 5/7/9/11. The numerical
preview transfers B's measured input voltage to the two C sources, rotating the
second by 90 degrees. That is an explicit prospective input assumption, since C
has not yet been measured. The primary C score uses the subsequently measured C
complex inputs with the same frozen coefficients and predictor. Both the prior
numerical prediction and this conditional prediction are retained. No C output
can enter coefficient extraction, and missing terms are never zero-filled.

## Acquisition and operational record

Full local files are under `build-reference/op1db-pilot-20261015/runs/` in
exclusive A/B/C/D directories. The dedicated saved reference SHA-256 is
`21e4b28eadd10919537434bc2e706d17fcf41e8691cc9406dea36ca69464487f`.
The original Blank session was first preserved as a separate workspace. A direct
COM SaveAs attempt did not create a backup and stopped before closing anything;
the repository's VBScript SaveAs approach then created the backup successfully.
The failed preparation attempt and both session inspections remain local.

A/B required exactly two analysis submissions. The separate
[unit reader](../scripts/reference/capture-cascade-parameter-units.ps1) performs
only readbacks and records its own hash. No security setting or credential was
changed. The frozen wrapper's existing process-only RemoteSigned invocation was
used unchanged. The reference collector compiled under Windows PowerShell 5.1;
an earlier PowerShell 7 compilation check had failed and is not counted as a pass.

The first native replay attempt rejected an executable/manifest identity mismatch.
MSBuild had loaded the old project definitions before its CMake regeneration;
building again after configuration stabilized produced the matching executable.
These are preparation/runtime-identity failures, not failed or retried SystemVue
analyses. No tolerance or vendor observation was altered to resolve them.

## Status at pre-C registration

The acquisition guard suites passed 11 and 17 tests. The new identification and
holdout-separation suite passed 17 tests against actual A/B records. The native
protocol/isolated-replay suite also passed all 33 tests, including the new suite
under an active product-package import ban. The independent direct-array and
multinomial audit verified all 380 preview origins with maximum relative error
8.9e-16. C and D are
pending at this registration. C must use the committed frozen artifact and the
same native executable/manifest for its first score. D is an end control and adds
no independent predictions. Complete RFAMP/network compatibility remains open.

The [A/B archive](../validation/rfamp-op1db-pilot-ab-20261015.json) and
[prediction freeze](../validation/rfamp-op1db-pilot-freeze-20261015.json) were
written at 2026-10-15 09:46:09 UTC before either C/D run directory existed.
The frozen prediction SHA-256 is
`882094c660e6a9f60e68a59f928ffe67d30614b762d0edc786ac59a66bab0d6a`.
The [independent audit](../validation/rfamp-op1db-pilot-freeze-audit-20261015.json)
also checked the selected A/B nodes against the complete original local captures.
