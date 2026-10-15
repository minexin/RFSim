# RFAMP OP1dB local pilot, 2026-10-15

This stage follows the unchanged [four-run acquisition plan](../validation/rfamp-op1db-pilot-plan.json).
A/B/C/D have been acquired from the dedicated `RFModel_CascadeIntermods.wsv`
copy with SystemVue 2023.0.0.11903. All four acquisitions are warning-free, and their
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
second by 90 degrees. That was an explicit prospective input assumption, since C
had not been measured at registration. The primary C score uses the subsequently measured C
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

A/B/C/D required exactly four analysis submissions, with no analysis retries. The separate
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

## Pre-C registration and completed holdout

The acquisition guard suites passed 11 and 17 tests. The new identification and
holdout-separation suite passed 17 tests against actual A/B records. The native
protocol/isolated-replay suite also passed all 33 tests, including the new suite
under an active product-package import ban. The independent direct-array and
multinomial audit verified all 380 preview origins with maximum relative error
8.9e-16. C and D were pending at registration. Commit
`1ba041510e91d35c3d389a67a04be5822b1f92b1` preserved that state and was pushed
before C. The first C score used the frozen artifact and the same native
executable/manifest. D is an end control and adds no independent predictions.

The [A/B archive](../validation/rfamp-op1db-pilot-ab-20261015.json) and
[prediction freeze](../validation/rfamp-op1db-pilot-freeze-20261015.json) were
written at 2026-10-15 09:46:09 UTC before either C/D run directory existed.
The frozen prediction SHA-256 is
`882094c660e6a9f60e68a59f928ffe67d30614b762d0edc786ac59a66bab0d6a`.
The [independent audit](../validation/rfamp-op1db-pilot-freeze-audit-20261015.json)
also checked the selected A/B nodes against the complete original local captures.

C started at 09:57:08 UTC, after the freeze at 09:46:09, registration at 09:53:59,
and final guard at 09:56:50. Its actual source-1 input equals the A/B value;
source 2 is `3.4433474765713634e-18 + j0.056234131816108215 sqrt(W)`.
The native common input scale is `0.9990897386142779`. No C observation was used
to fit or change a coefficient, source formula, or tolerance.

| C origin order | Independent origins | Passed | Maximum relative complex-wave error |
| --- | ---: | ---: | ---: |
| 5 | 28 | 28 | 6.073e-14 |
| 7 | 60 | 60 | 8.527e-14 |
| 9 | 110 | 110 | 1.094e-13 |
| 11 | 182 | 182 | 1.337e-13 |

All 380 origins pass the original `1e-7` tolerance, with no missing or unexpected
origins. Each origin has two band-edge observations; those edges are not separate
independent predictions. The preregistered numerical preview also passes 380/380.
The [original C score](../validation/rfamp-op1db-pilot-c-score-20261015.json)
and [measured C archive](../validation/rfamp-op1db-pilot-c-20261015.json) remain separate.

D started at 09:58:16 UTC and restores the A commands, 124 parameter entries,
units and analysis settings. Its 214 selected complex-wave band-edge rows and
coherence groups agree exactly with A across ports 1, 3 and 2. The broader raw
F/P/V/Z arrays also agree exactly. Raw numeric spectrum lookup IDs were renumbered
by SystemVue; resolving each through its original IDNo/IDName table gives identical
origin names, including coherence groups. The independent audit's initial
literal-ID comparison and its failure are retained locally; no physical value or
tolerance changed when the audit was corrected to compare the mapped identities.

The [completed replay](../validation/rfamp-op1db-pilot-replay-20261015.json)
includes a separate direct-array multinomial audit: 380 origins / 760 rows,
maximum error `1.343e-13`, and verification of all 24 original files. The
[completion record](../validation/rfamp-op1db-pilot-completion-20261015.json)
binds the four directories, original scorer, unit-reader source, saved workspaces,
packet manifests and restored-session evidence. The dedicated original workspace
hash is unchanged. The prior Blank session was restored after saving the final D
workspace separately; SystemVue remained open with no manager errors.

The [replay tool](../scripts/reference/report-rfamp-op1db-pilot.py) and ten regression
tests check the completed observations, immutable registration, changed data,
changed support, end-control drift, time ordering and absence of post-C fitting.
CI repeats the native replay on archived observations; it does not run SystemVue.
The [local regression record](../validation/rfamp-op1db-pilot-validation-20261015.json)
records five passing Debug CTest entries (including isolated native replay) and
four passing Release entries. The requested native DC and frozen-small-signal
executables pass 316 and 69 checks in both configurations.
This is one OP1dB profile's first-stage generated odd-origin holdout. It does not
identify the proprietary coefficient law or establish complete RFAMP, cascade,
or vendor-model compatibility.
