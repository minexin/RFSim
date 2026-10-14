# RFAMP first-stage OP1dB pilot: frozen acquisition preparation

This is cloud-tested acquisition tooling, not new SystemVue evidence. No local
computer, SystemVue session, license or user workspace was accessed. No proprietary
coefficient formula is supplied. Python and PowerShell here are developer evidence
tools, not product runtime interfaces; native code is unchanged.

## Why only one new control

The dedicated cascade collector already controls the required 1.0/1.1 GHz sources,
5 dBm powers, 0/90 degree phases, order 11, second-stage gain −10 dB,
RISO 140 dB, source BW 1 Hz, analysis BW 1 MHz and spectrum settings.
`--cascade-first-op1db-dbm` / `-FirstOp1dbDbm` is the only added physical
control. Omission retains 20 dBm. Second-stage OP1dB remains 20 dBm; all other
collector defaults remain unchanged. Negative dBm is valid: rejection is based
on non-finite values or failure to represent finite positive SI watts, not an
invented vendor dBm range. Vendor rejection/warnings stop acquisition.

Both stages' OP1dB SI readbacks are required, alongside the existing complete
parameter/topology/version/freshness checks during archiving. Ordinary old-profile
scorers reject a changed profile instead of silently reusing old coefficients.
The acquisition-only validator explicitly permits the override. Historical JSON
and measured-input archives are unchanged, with their original provenance.

## Frozen experiment

The authoritative plan is [rfamp-op1db-pilot-plan.json](../validation/rfamp-op1db-pilot-plan.json).
Its SHA-256 is embedded in the preparation tool. Its four ordered runs are:

1. A-baseline: first-stage OP1dB 20 dBm; one 1 GHz source, 5 dBm, 0°.
2. B-op1db21: only first-stage OP1dB becomes 21 dBm; same source.
3. C-two-tone-holdout: OP1dB 21 dBm; 1.0/1.1 GHz, each 5 dBm, 0°/90°.
4. D-restore-baseline: repeat A, an end control, not another independent prediction.

All fixed values and the four-analysis budget are recorded in the plan. Preparation
generates four separate scripts rather than an automatic batch. There is no
automatic retry, coefficient fit, nominal-input substitution or compatibility pass.

## User-run steps on the existing Windows reference setup

Use the repository with these exact files, existing Python developer tools and
SystemVue 2023.0.0.11903. Keep the original vendor project protected. Open only the
existing dedicated `build-reference/RFModel_CascadeIntermods.wsv` reference copy
in SystemVue, with no other workspace loaded and no outstanding collector or
analysis. The collector attaches to this session; it does not open a workspace.
Keep the saved copy unchanged between runs. No software installation, security
setting change, credential entry or execution-policy change is part of this procedure.

The following example assumes the checkout is `D:\RFModel`; substitute its actual
absolute path consistently. Use new, empty preparation, run and packet paths.

```powershell
cd D:\RFModel
python scripts/reference/rfamp-parameter-pilot.py prepare --windows-root D:\RFModel --windows-output D:\RFModel\build-reference\op1db-pilot-runs --output build-reference\op1db-pilot-prepared
```

Review `manifest.json`, `plan.json` and the generated scripts. Preparation hashes
the collector, wrapper and transitive validator sources. Each generated script
checks these hashes before calling the existing wrapper. The wrapper records its
command, snapshots collector/runner bytes, retains stderr and records the submitted
VBScript hash. Every output directory is exclusive, preserving previous attempts.

Run each command separately and inspect its `status.json`, `stderr.log` and raw
`capture.json` before proceeding. `captured` means the acquisition returned with
fresh data, not that compatibility passed. Complete pilot validation occurs when
archiving; inspect the full parameter and version readbacks before starting the
next run. Any warning/error/mismatch stops this experiment.

```powershell
& .\build-reference\op1db-pilot-prepared\A-baseline.ps1
& .\build-reference\op1db-pilot-prepared\B-op1db21.ps1
```

Pause after B. Extract A/B coefficients using the measured input reference and
existing common-limiter convention. Evaluate the null of unchanged coefficients
with the changed common response; retain residuals. Save and hash the B coefficient
file, extraction code, normalization and C prediction before examining C. This
tooling prepares acquisition and does not implement or certify that analysis or
analyst blinding. Do not repurpose the old fixed-profile scorer for B/C.

Only after that freeze, separately run:

```powershell
& .\build-reference\op1db-pilot-prepared\C-two-tone-holdout.ps1
& .\build-reference\op1db-pilot-prepared\D-restore-baseline.ps1
python scripts/reference/rfamp-parameter-pilot.py archive --preparation build-reference\op1db-pilot-prepared --runs build-reference\op1db-pilot-runs --output build-reference\op1db-pilot-packet
```

D requests the baseline again; no restoration has been performed by publishing
these instructions. After successful acquisition, preserve files and restore the
reference session according to the existing reference-copy procedure. On failure,
stop; do not run D as an extra recovery analysis. Preserve all partial files. If
status is `timeout_unresolved`, first inspect the reported collector PID and
SystemVue, because analysis may still be running. Never automatically retry,
kill the process, resubmit or close its workspace. Once no operation is running,
the user can return to the unchanged saved reference copy without another analysis.

## Evidence and analysis boundary

The packet keeps byte-identical capture/status/stderr and collector/runner snapshots
for all four run IDs, with SHA-256 bindings. It retains version, workspace hash,
submitted script/hash, timestamps, warnings, all raw parameter expressions/SI
readbacks, units/dimensions as supplied by the collector, complete raw
F/P/V/Z/ID arrays for ports 1/3/2, IDNo/IDName, RFElemList and RFPwrIn. The indexed
capture adds full port-1 nodes to the existing reduced cascade format; the existing
input-reference format additionally retains both measured source band-edge samples.
Missing required metadata or vectors fails rather than producing a partial pass.
Raw complex impedances are retained; existing validators reject unexpected
non-50-ohm/complex reference conditions rather than substituting a constant.

Use actual complex `V1_peak/sqrt(2*50 ohm)` for the fixed-reference input and common
limiter drive. Neither nominal 5 dBm nor `sqrt(P1)` replaces measured voltage.
Output comparisons retain `V3_peak/sqrt(2*Z3)` and independent V/P/Z checks. Compare
first-stage generated terms at port 3; port 2 is the second-stage output. Freeze
B coefficients before the C holdout and retain the original 1e-7 relative complex
wave tolerance. Missing/low-power terms stay missing; a failed prediction is
reported rather than refitted or repaired by widening the tolerance.

This four-run pilot can discriminate one-parameter dependence and test transfer
at one new profile. It cannot identify the proprietary law or establish automatic
RFAMP, full-network, merged-total, secondary-spectrum, noise or spectrum-reduction
compatibility. A/B differences can reflect common-response mismatch, not just
coefficient changes. A and D never increase an independent prediction count.

## Offline checks

```text
python -I -S scripts/reference/test_rfamp_parameter_pilot.py
python scripts/reference/test_run_systemvue_reference.py
```

The pilot suite uses explicitly mutated archival fixtures only inside temporary
directories. It checks frozen command generation/source binding, preserved defaults,
first/second-stage readback mismatch, finite/positive conversion, invalid inputs,
run identity/order, missing/corrupt evidence and full port-1 preservation. It is
registered in the developer-only native reference CTest group. Existing runner
Windows-only tests remain skipped on Linux. A Windows-only test parses the collector and generated PowerShell scripts through
the PowerShell AST parser without invoking them or changing execution policy; it
is skipped on Linux. PowerShell/C# compilation, COM behavior and live SystemVue
execution remain unverified here.
