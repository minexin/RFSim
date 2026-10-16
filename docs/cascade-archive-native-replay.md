# Fixed two-stage source-cubic archive replay (developer only)

This is a new, versioned diagnostic replay of eight existing SystemVue 2023 captures. It does not run SystemVue, restore product Python or a JSON graph dispatcher, or extend either existing source-only protocol. The frozen cascade checker, all RFAMP LOGIC_SOURCES, reference_records.py and native_replay.py remain byte-identical. The new scorer calls only frozen pure capture inspection/observation helpers; it never calls or monkeypatches the retired graph branch.

## Mathematical contract and predictors

The first stage is G=10 dB, OP1dB=20 dBm, saturation=23 dBm, IIP2=30 dBm, IIP3=20 dBm. The second has the specified G, the same compression/saturation, IIP2=40-G and IIP3=30-G dBm. Reference impedance is 50 ohms. Original nominal CW waves are sqrt(W) phasors; they are not recalibrated from measured RFPwrIn.

Only source-kind carriers generate quadratic/cubic products at each stage. Every physical input, including existing harmonic/intermod distortion, contributes to total drive. Existing distortion is linearly propagated with the fundamental gain for that drive; it never becomes a nonlinear parent or a new source. This definition is not unrestricted polynomial cascading or secondary remixing, regardless of UseVolterra readback.

The probe separately executes two predictors:

- CoherentLimitedAmplifier::evaluate, then evaluate_cascade, returns raw stage inputs/terms: 56 first-stage and 102 second-stage archive paths.
- CoherentPipeline seed and two limited_amplifier operations, the latter propagate_distortion, returns 56 typed combined physical records. It also returns physical-root mappings and separate stage statistics.

Typed roots are resolved by the complete (group, bin, bandwidth, RF role) source key to an original source ordinal. Group numbers are implementation-local identifiers, never SourceN. Signed primitive multisets retain repetitions and opposite signs; +x,+x,-x is three factors, never simplified to +x. Raw stage paths remain distinct from combined identities. Comparing the typed result with the raw prediction sum is only an API/transport consistency check. The combined vendor oracle instead sums measured complex waves from the matching second-stage generated/conducted origins.

## Independent protocol and finite domain

Magic RFMODEL_CASCADE_ARCHIVE_REPLAY, version 1, allows only cubic_two_stage_v1. The request is spacing=100 MHz, second gain in [0,20] dB and one/two nonzero source components. Each component must have bin 10 or 11, bandwidth 1 Hz, source kind and a distinct nonzero group/bin. Source ordering and noncontiguous groups are supported for mapping controls. The archive scorer separately requires the exact historical eight-case matrix; synthetic protocol tests are not vendor evidence.

No arbitrary stage list, external second-stage distortion array, user coefficients, JSON model, generic propagation option, or source-clock graph is accepted. Existing RFMODEL_NATIVE_REPLAY v1 and RFMODEL_SOURCE_ARCHIVE_REPLAY v1 source-only domains remain unchanged.

Responses contain two raw stages (drive, limited drive, inputs, terms), a complete physical root/source map, typed combined records with signed source-ordinal factors, and two typed stage-stat rows. Count limits are source/root<=2, stage1 input<=2/term<=16, stage2 input<=16/term<=30, combined<=16, factors<=3. The complete path/combined count is <=62. Requests/responses are <=65536 bytes and tokens <=128 bytes. Only finite decimal values, bounded integer types and ASCII are accepted; nonzero underflow-to-zero, excess data, duplicate/missing identity, unknown root, incorrect kind/support, altered drive, and secondary-parent generation fail closed.

The process transport is reused from the frozen runner, with an independent envelope/operation decoder. It verifies request/build identity, timeout, bounded stdout/stderr, return-code/status agreement and executable/manifest hashes before and after execution. Windows stdio is binary to avoid CRLF folding or Ctrl-Z truncation. The SDK receives no new parser or artifact. A real second TU includes/invokes the pipeline and links into the same probe, both in-tree and after pure-SDK installation/relocation.

## Measurement and missing-evidence boundary

All eight archives lack UseSpecReduction, ElimSpec and IgnorePwrLvl readback. Each report explicitly records assumed_value=true, assumption_source=legacy_checker_default, vendor_readback_verified=false and eligibility=diagnostic_only. Physical acceptance requiring that setting remains blocked. No subsequent collector or unrelated study fills this historical gap.

The selected port3 stage1 origins exclude 43 measured origins with the RFAmp2,RFAmp2 suffix. Their observed identity/boundaries are preserved, but direction is unknown; no reflection/reverse-drive conclusion or missing-zero substitution is made. The RISO=140 case lacks that family. This is neither full two-port path coverage nor NodeTotal acceptance.

The archived V values are peak complex voltages: |V|²/(2 Re Z)=P must agree, Z must be real 50 ohms, and wave=V/sqrt(2 Re Z). CW boundaries are flat and exact. The 1 Hz ChanBW case restores whole-term wave by dividing by sqrt(min(1, ChanBW/termBW)). No boundary powers are summed or integrated. Source BW is 1 Hz and degree-two/three origin bandwidth is 2/3 Hz.

The current capture/report bytes and frozen scorer closure are checked against literal LF hashes; CRLF checkout equivalence is recorded without rewriting files. The historical capture hash is reproducible by an in-memory LF-to-CRLF conversion, not by recovering raw measurements. Eight original raw captures, original workspace and historical library are historical_declared_unverified. DataEntry/SI pairs and Model/Netlist are inspectable, while Unit:Long attribute names do not provide numeric Unit IDs or a post-run in-memory workspace snapshot.

## Versioned verdict

Each replay computes current results afresh and stores current/history/delta by complete measurement identity. The historical prediction columns are comparison data only. All 214 spectral checks jointly require relative complex error<=1e-7 AND absolute relative power error<=1e-7; 16 drive checks require relative error<=1e-7. The expected historical diagnostic is 230 checks, 208 pass and 22 fail, with failures [11,11,0,0,0,0,0,0]. Ten failures exceed only the power threshold. Neither unity-gain fitting, noise injection nor threshold changes are permitted.

The regression gate tests preservation/reporting of known discrepancies, not vendor compatibility. Any actual status change is retained and investigated, never overwritten with historical status. Reports are atomically published only at a new output path. Current source/content/build hashes, compiler/configuration, request/response hashes and readable archive/scorer hashes are recorded.

## Focused development checks

Build rfmodel_reference_cascade_probe, rfmodel_reference_cascade_oracle and (for fault transport controls) rfmodel_reference_highorder_oracle with RFMODEL_BUILD_REFERENCE_TESTS=ON. Run ctest -R '^native_cascade_' separately for strict Debug and Release. The installed check configures SDK with testing/C API/CLI/examples/Python/reference tests disabled, rejects a C compiler/Python discovery or leaked developer files, relocates the SDK, and builds the actual two-TU consumer using only RFModel::rfmodel.

The independent oracle has no RFModel headers and computes scalar compression/limiting and ordered signed Fourier products in long double. It is a mathematical oracle, not a vendor source. Tests cover archive recipes plus bounded unequal/reordered/noncontiguous/saturated/phase synthetic inputs, protocol envelope/count/byte errors, waveform/kind/origin/root/support mutations, cold source-tamper rejection, import guards, and historical power-only failures. Full existing regression gates and remote MSVC/macOS CI are separate stages; local focused checks cannot establish those results.
