# Touchstone 2.0 native import validation, 2026-10-13

This records local Linux/GCC 14.2.0 validation of the bounded import work atop
`4303efb801873cd5afe4a747bacd4fcdd4e35d30`, including the published pipeline
adapter allocation-test portability split, before Touchstone commit/publication. The
[JSON evidence](../validation/touchstone-two-validation-20261013.json) contains
SHA-256 hashes of every tested implementation/registration/test file.
The [API contract and supported subset](touchstone-two.md) distinguish this work
from full format or vendor compatibility.

## Integrated rerun on 4303efb8

| Configuration | Result |
| --- | --- |
| Full Debug, including Python and native archived-reference regressions | 174/174 |
| Full Release, including Python and native archived-reference regressions | 174/174 |
| Installed Debug C/C++ consumers | 6/6 |
| Installed Release C/C++ consumers | 6/6 |
| Native Release, C ABI and Python binding tests disabled | 107/107 |
| Minimal header-only SDK, Python discovery disabled; installed Release C++ consumers | 4/4 |

Native archived-reference tests use a Python fixture generator at configuration
time. The separate minimal SDK run disables those developer-only replays, C ABI,
CLI, examples and tests during installation, and does not discover/require Python.
Its installed consumers use only the installed include directory with C++17 and
`-O3 -DNDEBUG`.

New C++ tests perform 490 numeric checks and 71 malformed/unsupported-file
rejections. Maximum physical/kT-scaled error is 6.0126956048082556e-15, below
3e-12. New C tests add 68 numeric checks, with maximum scaled error
1.5031739012020639e-15, plus failure-output and handle checks. Assertions remain
active in Release. CLI rejects unequal references before writing CSV and reports
a common `[Reference]` override correctly.

An independent `-O1 -DNDEBUG` AddressSanitizer/UndefinedBehaviorSanitizer run on
the historical 55ab9d90 snapshot passed the complete native test. Its Touchstone
parser and test blobs are unchanged, but no fresh independent sanitizer run on
the integrated base is claimed. LeakSanitizer could not run in the ptraced execution
environment and was disabled. An initial vertical-tab/form-feed-only line crash
was fixed, then covered by parser rejection and sanitizer regressions.

Three isolated, deliberate Release mutations each failed the independent test:

- Swapping the full two-port direction: nonreciprocal off-diagonal check failed.
- Conjugating mirrored off-diagonal triangular elements: complex-symmetric check failed.
- Rescaling already-ohmic 2.0 Rn by option R: raw-Rn units check failed.

The resistor and noisy amplifier fixtures use direct physical V/I and wave
algebra, including a separate passive thermal covariance oracle. Product
renormalization is not used to manufacture their expected results.

Changed C/C++ files pass clang-format 19.1.7 and `git diff --check`.

## Historical runs retained

Earlier matrices passed atop `55ab9d90ac6a44ce2339291297483ce2c3304ade` and
`bf5a5560e097eb48551316bc065180c1a2ecbb2c` (each: 173/173 Debug and Release,
6/6 installed consumers in each, 106/106 native, and 4/4 minimal SDK). Their
original source hashes, validation details and resolved initial failures remain
in `historical_runs` in the JSON evidence. They are not substituted for the
integrated rerun above. The extra current test is the separate allocation test.

The integrated run verifies that `coherent_pipeline.hpp` with PendingStore,
`coherent_pipeline_amplifier_limits_test.cpp`, and
`coherent_pipeline_allocation_test.cpp` are byte-identical to 4303efb8. Both
300-second CTest timeouts remain. The baseline MSVC iterator-debug override stays
private to the allocation target; default settings elsewhere are unchanged. No
pipeline implementation or test changes are included in the Touchstone patch.

## Not established by this record

No new SystemVue measurements, Windows/MSVC or macOS/AppleClang run, remote CI,
Touchstone commit, push, merge or publication is claimed. Prior platform results
remain tied to their original commits. The integrated base includes the separately
reviewed allocation-test portability split following the bf5a5560 Windows Debug
failure. These Linux passes do not certify its Windows behavior. Touchstone
publication remains held pending exact-commit remote validation of that separate
change; no unobserved platform result is counted as a Touchstone pass.
