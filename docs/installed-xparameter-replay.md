# Read-only installed X-parameter format replay

This **developer-only** tool builds the existing native reader probe against an
explicitly installed RFModel SDK and checks four selected installed example files
against previously recorded format evidence. It does not execute SystemVue, COM,
or any simulation; it does not establish numerical model compatibility, units,
normalization, or physical reference planes. Python is an orchestration/scoring
utility using only the standard library, not a product interface or vendor parser.

## Prerequisites and invocation

Use Python 3.9+, CMake 3.20+, an installed C++17 compiler, and an existing header-only
RFModel 0.1.0 SDK containing `lib/cmake/RFModel`. The tool never installs missing
software, searches other SystemVue installations, or falls back to repository
headers. Missing prerequisites produce a blocked result. Other package layouts are not supported by this tool; do not point it at source
headers or install anything as a fallback.

Example (quote paths with spaces; use your actual installation and SDK paths):

```text
python scripts/reference/replay-installed-xparameter-evidence.py --installation-root "C:/Program Files/Keysight/SystemVue2023" --sdk-prefix "C:/RFModel SDK" --output-dir "C:/replay-results/run-001"
```

The output parent must already exist, while `run-001` must not exist. An existing
output directory is rejected without changing it. Output must be outside both
input roots. Optional `--cmake`, `--compiler`, `--generator`, `--config Debug|Release`,
and `--timeout` (per selected file, at most 300 seconds) select existing tools.
The default configuration is Release; commands are invoked as argv, never via a
shell. An ordinary Windows developer shell may be needed for its compiler's
standard-library environment. A single executable path/name is supported for `--compiler` or `CXX`; launchers
or embedded compiler arguments are blocked, as are nonempty `CL`/`_CL_` overrides.
Process-local include/flag override variables are cleared, without changing global
settings or Windows `INCLUDE`/`LIB`/SDK environment. The installed reader header is
selected by absolute path. This protects against accidental source-header fallback;
it is not an assurance against a malicious compiler or host.

The exact input directory, relative to the installation root, is:

```text
Examples/RF Architecture Design/RF Design Kit/X Parameters/
```

Only these files are read, each at most once and each passed to the probe once:

1. `ZFL_11AD_PHDModel.mdf`
2. `X-param_BalAmp_ZFL11AD.mdf`
3. `Multitone_Volterra_X-device.xnp`
4. `AMP_Xparams.mdf`

No other vendor data, QCH help databases, README, installation registry, or account
information is accessed. No complete vendor file is copied into the results.
The installed SystemVue version is reported as `unknown`: this tool does not have
a verified cross-platform version detector. The historical baseline's version is
recorded separately and must not be read as a freshly observed version.

## Identity, bounds, and exact scoring

Each file is read into at most 64 MiB of memory. The same bytes supply SHA-256 and
the native stream reader via binary stdin (including on Windows). Device, inode,
mode, size and mtime bind the opened descriptor to the selected path. On Windows,
Python's pathname stat may expose creation time as ctime while descriptor fstat
exposes metadata-change time ([upstream issue](https://github.com/python/cpython/issues/157671)),
so ctime is not compared across those two APIs;
other platforms retain the complete cross-API comparison. The full descriptor
identity (including ctime) is checked against its own post-read observation, and
the full pathname identity against its own post-read observation. Detected changes
fail without retry. This is a consistency check, not a filesystem snapshot
or a guarantee against a privileged concurrent writer restoring metadata.

The native parser must finish before a JSON summary is emitted. Schema 1 uses
classic-locale, round-trip binary64 numbers, UTF-8 validation and strict JSON
escaping. Native output is limited to 256 KiB, 4,096 blocks/emitted records, 256-byte
names and 16 KiB strings. Bound excesses fail instead of truncating a successful
summary. Diagnostics contain fixed text and source locations, never raw input
lines. Subprocess stdout/stderr captures also have hard collection limits.

Summaries preserve ordered coefficient row/column counts, typed ordered VAR/#
declarations and metadata rows, first coefficient block family counts, and at most
five first-row FI_/FB_2_/S_2_/T_2_ samples from that block. AN min/max/first use
coefficient **row fields** only; outer VAR AN values are excluded. `first` follows
source order and need not equal the minimum. Archived `legacy_constants` retains
only nonempty constant groups in order: the old evidence did not retain empty
groups or original block indices, so this comparison cannot independently verify
a constant group's attachment to a particular coefficient block. The native reader validates all
physical data groups, not only the emitted sample.

The immutable comparison source is
`validation/systemvue-xparameter-format-evidence-20261015.json`, SHA-256:

```text
432022e69d4cb1010b27d73b18adc03488d106989f69900ccc5592421963ec59
```

The scorer checks this digest before projecting its small, fully matched VAR/#
declarations to typed expected records. Metadata types are explicitly projected
from the archived schema (Version is real; counts are integers; RefZ0 is complex;
PortName is string), never guessed from whether a JSON value happens to be integral.
Unsupported declarations/fields fail. Candidate vendor files are parsed only by
the C++ reader. No expected result is regenerated from observed native output.

All four exact byte lengths and hashes must match before shapes, names, types,
order and values can pass. There is no tolerance, fitting, or numeric model score; signed zeros are distinct.
Boolean values are never accepted as numbers. Missing/extra fields, duplicate JSON
keys, extra stdout, nonfinite values, unexpected schema, timeouts, nonzero exits,
or input changes fail closed. A different vendor file version is an identity
mismatch, not an invitation to replace the expected baseline.

## Evidence and interpretation

`replay.json` retains status, tool/source/package/header/executable SHA-256 values,
compiler/Python/CMake versions, command argv/times/status, baseline identity,
selected-file identities and bounded summaries. Sources, baseline, stdout/stderr,
and per-command receipts are kept under the exclusive output directory. Build
products remain there too. Failure evidence is retained; nothing is automatically
cleaned up or retried. A failed file does not prevent the other planned files from
being attempted once. Handle output as local evidence and review it before sharing.

Only `selected_installed_file_replay_pass` means that these exact four selected
installed files matched the old format evidence through the installed native
reader. Only the registered schema/count/sample fields are compared; unregistered
coefficient values are not independently validated. They represent **three unique content hashes** because the two legacy
files are byte-identical. Independent simulations remain **zero**, and
`numerical_model_compatibility` remains **false**, including on success.

Cloud synthetic tests are never an installed vendor-file pass. Current automated
coverage uses authored synthetic legacy/v2 fixtures, strict scorer mutations,
installed-only dual-translation-unit compilation, output protection, retained
failures, timeouts, one-shot collection, and separate pathname/descriptor ctime
mutation tests. A real synthetic fixture changes metadata before collection to
exercise Windows' differing creation/change timestamps. Complete vendor files are not committed
or uploaded by this workflow. Actual four-file local replay remains a separate
validation step. The first local attempt on 2026-10-16, using Python 3.12.14,
failed all four files before native parsing because of the Windows ctime API
difference described above. That failed receipt is retained; it is a collection
defect, not evidence of a vendor file format or baseline identity mismatch.
The corrected collector's separately retained [local replay on 2026-10-16](local-takeover-validation-20261016.md)
passed all four selected files against the unchanged baseline (three unique content hashes,
zero independent simulations). That result closes this selected-file format check only.

## Developer test gate

With `RFMODEL_BUILD_REFERENCE_TESTS=ON`, CTest registers
`xparameter_installed_replay_contract`. It makes a fresh header-only SDK installation,
builds the dual-TU probe using installed headers, and runs the synthetic Python
suite. Python is required only for the opt-in developer reference tests; ordinary
product/native SDK consumers do not require it. CMake SDK/probe workspaces are retained; the Python suite cleans its temporary synthetic fixtures.
The gate supports Debug and Release without rebuilding the numerical test suite.

The standalone suite can also run with `RFMODEL_XPARAMETER_PROBE` and
`RFMODEL_XPARAMETER_SDK` set to existing local test artifacts. Without them it explicitly
skips those integration portions; such a partial run is not the full gate.
