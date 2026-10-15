# SystemVue DC and small-signal follow-up, 2026-10-15

**The vendor comparison remains incomplete.** Exact code commit
`dddc0593961b3d734ee7702d96ee9d4ca370519a` passed all 18 Windows/Ubuntu/macOS
Debug/Release core, SDK and archived-replay jobs in
[CI 38722607277](https://github.com/minexin/RFSim/actions/runs/38722607277).
The [CI record](../validation/dc-phd-ci-dddc059-20261015.json) contains the run and
individual job outcomes. This follow-up changes evidence and documentation only;
it does not attribute that CI run to a later evidence commit.

The live investigation submitted eight dedicated analyses in SystemVue
2023.0.0.11903. None establishes an accepted DC or small-signal comparison.
No RFAMP analysis was repeated, and no native code, circuit acceptance contract,
or tolerance was changed.

## Evidence and scope

- [Structured findings and raw voltage extract](../validation/dc-systemvue-followup-20261015.json)
- [Raw archive](../validation/dc-systemvue-followup-raw-20261015.zip): 147 files,
  including input/result workspaces, COM captures, preparation records, procedures,
  error logs and restoration evidence. Every archived byte stream was checked
  against its SHA-256; the ZIP CRC check also passed.
- [File manifest](../validation/dc-systemvue-followup-manifest-20261015.json):
  archive hash, per-file hashes, and ten vendor documentation files retained locally.
  Full vendor manuals and extracted help pages are not redistributed in the ZIP.

The complete working evidence remains under
`build-reference/dc-systemvue-followup-20261015`. The archived scripts are a record
of attempted procedures, including unsuccessful ones; they are not an unattended
acquisition workflow. The earlier [readiness record](../validation/dc-bias-systemvue-readiness-20261015.json)
is retained unchanged as the snapshot before these submissions.

## Model entry and product boundary

The live model registry contains NLRES and NLCAP. `ChangeModel` initialized their
parameter metadata, and the coefficient arrays were read back through `GetValue`.
NLRES identifies `TYPE=2` as polynomial and `MODELTYPE=0` as I(V). The candidate
current coefficients `[1, 0.001, 0, 0.002]` follow the leading-exponent convention
in the older [Genesys part catalog, p.241](https://edadownload.software.keysight.com/eedl/Genesys/2010_05/pdf/part.pdf).
That format clue does not establish SystemVue solver support.

The installed `Help/systemvue.qch`, resource
`sim/Non-Linear_Co-Simulation.html`, explicitly limits SystemVue nonlinear circuit
co-simulation to X-parameter models and directs general nonlinear circuit
simulation to Genesys. Its decompressed SHA-256 is
`aeb07a110db0c9c5cc481aeeda34b8c8b147940a33eed1f475dc003d1b034c1e`.
This is a documented boundary of the tested circuit path, not an inferred license
failure or a claim about every SystemVue API/version.

Actual linear-analysis submissions failed while creating NLRES and NLCAP models.
Successful metadata entry therefore did not yield a usable cubic device. NLCAP
accepted `[1, 1e-9, 0, 3e-9]` as an array, but whether it denotes Q(V) or C(V)
remains unverified. This was a separate model-entry probe on the dummy-port node,
not a completed combined cubic-current/charge bias fixture. The frozen input
netlists retain automatic node renumbering so the probes cannot be mistaken for
the accepted physical circuit.

## Analysis outcomes

| Submission | Observed outcome | Acceptance consequence |
| --- | --- | --- |
| Divider without RF ports | Failed: no ports. The initial script helper timed out, but later readback found the failed analysis log. | No solved DC V/I. |
| Divider with an NLRES equivalent-load candidate | Failed to create NLRES model. | No operating point. |
| Ordinary resistor control with a separate dummy RF port | Analysis returned F, S and ZPORT. | Confirms that the linear analysis executes; it does not establish divider bias or independent V/I. |
| Cubic current entry | Failed to create NLRES model for RL. | No cubic bias or derivative result. |
| Cubic charge entry | Failed to create NLCAP model for ChargeProbe. | No charge or dQ/dV result. |
| Flat two-node RC | Exported spectra; maximum order 1 was promoted to 3 with a warning. | Directional totals in a circuit with feedback loops are not accepted as complete physical node observations. |
| Pure linear RC subcircuit, initial output setup | Analysis completed with a warning that all spectrum output was disabled. | DataLevels/UseSourcePts had been serialized as parameter items instead of typed analysis properties. Failed setup retained. |
| RC subcircuit with corrected typed output properties | Completed without warnings; exported V1/V2 spectra. | Numeric and frequency/boundary checks failed, as below. |

Before these submissions, the serialized `DC Analysis` object type could not be
created. A separate GPOLY metadata initialization attempt also failed. These
construction failures remain in the raw archive and are not additional analyses.

The linear control did not expose solved node voltage or terminal current.
`IPROBE.IDC` and `TEST_POINT.VDC` stayed input parameters. Spectrasys `DC_V` and
`DC_I` are supply-budget measurements, not the desired nonlinear operating point.

## Independent RC voltage export

The installed `sim/Linear_Co-Simulation.html` recommends putting pure linear
components in a subcircuit when the circuit contains loops. The final setup places
C1, R12, C12, R2 and C2 in `RC_Core`, with Source and TEST_POINT outside it.
The physical source resistance is the MultiSource's 1000 ohm internal resistance,
included once. Each source has 0.25 mW available power, corresponding to 1 V RMS
Thevenin voltage. Parsed component values retain the original matrices:

```text
G = [ .003  -.002 ; -.002  .005 ] S
C = [ 5e-9 -4e-9 ; -4e-9 6e-9 ] F
```

The internal 50/75-ohm port references identify the subcircuit terminals; no
matched output port was added to the top circuit. V1 and V2 come directly from
the vendor voltage spectra, not from S-parameter reconstruction. Positive-frequency
complex peak voltage is divided by sqrt(2) for RMS. Source current is explicitly
**derived** as `(1-V1)/1000`; reduced Y including Rs is **derived** as `0.001/V1`.
There is no direct vendor current or Y channel in this acquisition.

Both the 1 kHz and 1 MHz source identities produced the same RMS values:

```text
V1 = 0.45451304687801275 - j0.0035808621699473775 V
V2 = 0.1818192938647407  - j0.0005190551513625725 V
```

The raw axes contain CW edges at 999.5/1000.5 Hz and
999999.5/1000000.5 Hz, not center rows, even with Pts=3 and the corrected
UseSourcePts property. The installed help describes center-frequency
characterization for this co-simulation. Applying that documented interpretation
to the unchanged analytic reference gives:

| Source center | Absolute V1 error | Absolute V2 error | Result |
| --- | ---: | ---: | --- |
| 1 kHz | 1.791370568e-6 V | 2.602292710e-7 V | Fail |
| 1 MHz | 4.474470448e-1 V | 1.771925960e-1 V | Fail |

The criterion remains `abs(a-b) <= 2e-12*max(1,abs(a),abs(b))`; here the voltage
threshold is 2e-12 V. Comparing against the lower edges also fails: for example,
the 999.5 Hz V1 error is 2.421312633e-8 V. No center interpolation, frequency
selection, amplitude fitting or tolerance change is used to turn this into a pass.

The exported Z2 toward TEST_POINT is 1e10 ohm. That output does not establish an
ideal nonloading probe. Its meaning, the boundary conditions and the repeated
response at two frequencies remain unresolved. The observation is insufficient
to assign a specific vendor implementation defect or a native RFModel defect.
There is no accepted physical V/I/Y result and no subsequent S comparison.

## Session restoration and remaining requirement

Every result was saved before restoring the initial empty session. The first
restoration helper did not return and was stopped without terminating SystemVue.
The official OpenWorkspace attempt returned an internal error, so its return
status alone was not counted as restoration. A subsequent independent COM read
confirmed the original empty design and equations, idle analysis, and an empty
manager error log. Canonical XML content matches after excluding only window,
viewport, selection and timestamp fields. The original saved workspace and the
opened copy have identical bytes; both are archived with the recovery failures.

Completing the requested nonlinear comparison now requires a supported reference
backend or vendor example that accepts the exact I(V)/Q(V) laws and exports solved
node V and terminal I. The RC frequency and probe behavior must also be resolved
before those spectra are used as an equivalent physical reference. The existing
native tests and CI remain valid mathematical checks; they do not close these
vendor acceptance conditions.
