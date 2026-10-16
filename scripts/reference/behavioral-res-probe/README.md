# Installed C++17 RES-chain probe

Developer evidence tool for the frozen
[`systemvue-linear-rfsystem-minimal-20261016-v1`](../../../docs/systemvue-linear-rfsystem-minimal-plan-20261016.md)
experiment. It does not launch SystemVue or grant vendor acceptance.

The same owning `ModelAdapter` instances and connection list feed
`analyze_linear` and `analyze_conversion_simulation`. The existing
`analyze_linear_path` also produces explicitly named power reference planes.
Two 50-ohm series resistors are driven by one 100-MHz, zero-phase source with
1-mW available power, 50-ohm source and 50-ohm load. Intrinsic noise is
explicitly omitted. No CP, CV, CNV or vendor impedance aliases are supplied.

`main.cpp` and `fixture.cpp` are separate translation units, exercising the
installed headers together. CMake requires an absolute `SDK_PREFIX`, constrains
the package and its include directories to that install, and generates absolute
installed entry-header includes. It supplies no source-header fallback.

The executable writes one finite JSON document to stdout using binary64
`max_digits10`. It includes full complex S and reference impedances, named
device-port RMS V/I, incident/outgoing power waves, net power positive into each
device port, and source-available/input-accepted/load-delivered power. Loaded
single-CW peak voltage is explicitly derived from the physical RMS phasor; it is
not claimed to be a general RF System channel voltage. Node MID occurs twice,
once per connected device terminal, with opposite entering-current signs.

From the repository root, using explicit installed CMake and Python paths:

```powershell
& $Python scripts/reference/behavioral-res-probe/run.py `
  --repo D:/project/RFModel --cmake $CMake `
  --out D:/project/RFModel/build-reference/res-native-new-run `
  --generator 'Visual Studio 17 2022' --architecture x64
```

`--out` must be fresh. The runner installs a fresh header-only SDK, configures
separate Debug/Release consumers, builds, executes, verifies both results and
checks twelve deliberately incorrect results. No product Python interface is
required. Use `--configuration Debug` or `--configuration Release` for one CI
matrix entry; the option is repeatable, preserves selection order and ignores
duplicate selections. Omitting it validates both configurations. Negative
checks use the last configuration actually validated, including a Debug-only
run; the receipt records that choice.

On other platforms omit `--architecture`; choose an available CMake generator
or let CMake select it. The runner explicitly installs to `lib` on every host,
matching the consumer's package location rather than a host-specific `lib64`.
Paths are handled as `pathlib.Path` and subprocess argument arrays; JSON and
generated CMake metadata are read as UTF-8. On POSIX the executable has no
`.exe` suffix. Only the observed platform/compiler is proven by a run receipt;
this fixture's presence does not claim unrun platform coverage.

`verify.py` loads the byte-hashed frozen plan and independently applies the
series-circuit equations and frozen full-S oracle. It rejects duplicate JSON
keys, nonfinite numbers, wrong identities, current/reflection signs, reference
plane confusion, and RMS/peak confusion. Its tight native-only bounds
(`atol=1e-15`, `rtol=1e-12`) do not replace the plan's vendor comparison policy.
The 99 checks include identity and shape gates, not 99 independent physics cases.

The local evidence directory retains exact command argument arrays and exit
codes, stdout/stderr, compiler identity, source and installed-file SHA-256
manifests, binary/result hashes and oracle receipts. MSVC runs additionally
validate every RFModel header in `/showIncludes` against the installed prefix.
The Windows environment is materialized explicitly to avoid inherited duplicate
`Path`/`PATH` entries. If sandboxed MSBuild FileTracker is denied, rerun under the
normal approved local-build permissions in a new output directory; preserve the
failed receipt.
