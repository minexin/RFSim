"""Versioned typed-native replay of the 23 shared-source historical captures.

Two unchanged scorers retain distinct verdicts: shared direct-carrier/drive and
single-stage cubic origins. Historical predictions are comparisons, never inputs.
No SystemVue run, cascade, noise fit, retired Library or product Python is used.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from native_source_replay import NativeSourceReplay
from native_replay import NativeReplayError, _sha256, atomic_write_report, forbid_product_imports

CAPTURES = "systemvue-2023-shared-compression-captures.json"
HISTORY = {"shared": "systemvue-2023-shared-compression.json",
           "coherent": "systemvue-2023-coherent-amplifier.json"}
ARCHIVE_SHA256 = {
    CAPTURES: "4a02f9213bf4a1324471e545dde8e8fd24c0ac2c078bb6c193799196ddcff2ec",
    HISTORY["shared"]: "73b743874ce6fbcf9f5a49bfdaa5cf4245c1ac80a74790f2bcb229df42c39978",
    HISTORY["coherent"]: "e4fd87f29dd8bf81c6de79f05a1c8820168a6894162bea53ee170484a13a2507",
}
# Complete transitive scorer closure, plus the deliberately unchanged retired
# cascade entry and old transport. Hashes are original LF bytes, not current
# values silently blessed at runtime.
FROZEN_SHA256 = {
    "compare-amplifier-terms.py": "a2427cc2934f89a91b0c51dd45aed8aac5004262bf93a1fdad5e6813425895c7",
    "compare-two-tone.py": "bd3a838ee9d4a1b1584da69eadfc1d050dee903b6d3e1be687c6cc367848605e",
    "compare-single-compression.py": "48c2eb810803fb5f659cf80ab06c43f9643e201f98e3d75cd1dc8e63f8541050",
    "run-systemvue-reference.py": "e3c7e9b6772031b21c8414a627a541495b3997ad8fd4c0754d7f7c5289190e67",
    "compare-shared-compression.py": "c1d4f6e91ed15f2958c7780ab7472573298f8a57cb389d4eb1d6b1fcf682d65b",
    "compare-coherent-amplifier.py": "a499bea303d687204ea5342d1cdb3d9a23be5d8d03515ea1ef7c08cbf75c6cbf",
    "compare-cascade-amplifier.py": "e340f4cb84e3722a0cd7d23d54cc7e83b7f1ef437559e79f4c0952ee20ccaf1a",
    "reference_records.py": "0933db4d825a58bc02ef155ce29a1053e2726d21aa5f6afee69bb263686fc63b",
    "native_replay.py": "a90ec8d335b0b5526abd284ee9243d6bac864de1b83cab20999d62044df7fc1a",
    "retired_binding.py": "b91041225bf98078c62ee1f1dd1b37c1f70f6ed6c655b46c55858cc0b9b747ae",
}
# Fixed archive coverage, not a formula for the number of terms in other inputs.
# P1, P2, phase2, same-frequency, locked, noise, diagnostic.
CASE_CONFIGURATIONS = (
    (-6, -6, 90, False, False, True, False),
    (-6, -6, 0, True, False, True, False),
    (-6, -6, 180, True, False, True, False),
    (-6, -6, 90, True, False, True, False),
    (-6, -6, 0, True, True, True, False),
    (-6, -6, 90, True, True, True, False),
    (-30, -30, 0, True, False, True, False),
    (-6, -26, 0, True, False, True, False),
    (-30, -30, 180, True, False, True, False),
    (-6, -26, 180, True, False, True, False),
    (-6, -26, 90, True, False, True, False),
    (-30, -30, 0, True, True, True, False),
    (-6, -26, 0, True, True, True, False),
    (-6, -26, 180, True, True, True, False),
    (-26, -6, 90, True, True, True, False),
    (-30, -30, 90, True, True, True, False),
    (-6, -26, 90, True, True, True, False),
    (-30, -30, 0, True, False, False, False),
    (-30, -30, 180, True, False, False, False),
    (-30, -30, 0, True, True, False, False),
    (-6, -6, 180, True, True, False, True),
    (-30, -30, 90, True, True, False, False),
    (-6, -6, 180, True, True, True, True),
)
CASE_FIELDS = ("source_powers_dbm", "source_phases_deg", "same_frequency", "locked", "noise_enabled")


def _load_checker(filename):
    spec = importlib.util.spec_from_file_location("source_archive_" + filename.replace("-", "_"), HERE / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def _validate_cases(captures):
    if not isinstance(captures, list) or len(captures) != 23:
        raise ValueError("Expected exactly the 23 shared-source captures")
    actual = []
    for capture in captures:
        if capture["source_phases_deg"][0] != 0:
            raise ValueError("Unexpected first-source phase")
        actual.append((*capture["source_powers_dbm"], capture["source_phases_deg"][1],
                       capture["same_frequency"], capture["locked"],
                       capture["noise_enabled"], capture["diagnostic"]))
    if tuple(actual) != CASE_CONFIGURATIONS or len({c["raw_capture_sha256"] for c in captures}) != 23:
        raise ValueError("Shared-source configuration coverage or identity changed")


def _component_key(component):
    return (component.bin, int(component.kind), component.bandwidth_hz, component.coherence_group)


def _component_json(component):
    return dict(bin=component.bin, kind=component.kind.name, bandwidth_hz=component.bandwidth_hz,
                native_group=component.coherence_group,
                amplitude_sqrt_w=[component.amplitude.real, component.amplitude.imag])


class _RecordingProvider:
    """Record actual requests/results without replacing either frozen scorer."""
    def __init__(self, provider):
        self.provider = provider
        self.shared_calls, self.coherent_calls = [], []
        self.pending = None

    def assign_source_coherence(self, definitions):
        groups = self.provider.assign_source_coherence(definitions)
        self.pending = (tuple(definitions), tuple(groups))
        return groups

    def _record(self, name, spacing, components, options):
        if self.pending is None:
            raise NativeReplayError("Missing source definition request")
        definitions, groups = self.pending
        self.pending = None
        components = tuple(components)
        result = getattr(self.provider, name)(spacing, components, **options)
        record = dict(definitions=definitions, groups=groups, inputs=components, result=result,
                      spacing_hz=spacing, parameters=dict(options))
        (self.shared_calls if name == "compress_coherent_fundamentals" else self.coherent_calls).append(record)
        return result

    def compress_coherent_fundamentals(self, spacing, components, **options):
        return self._record("compress_coherent_fundamentals", spacing, components, options)

    def coherent_amplifier(self, spacing, components, **options):
        return self._record("coherent_amplifier", spacing, components, options)


def _source_mapping(capture, call, shared, predicted=None):
    """Expose equivalence classes, retaining every contributing primitive source.

    The small phasor sum is provenance bookkeeping, not the native RF predictor.
    Source names are never inferred from a numeric group or strongest amplitude.
    """
    definitions, groups, inputs = call["definitions"], call["groups"], call["inputs"]
    if len(definitions) != 2 or len(groups) != 2 or len(inputs) != 2:
        raise ValueError("Both original source definitions and inputs are required")
    _, _, direct, _ = shared.inspect(capture)
    definitions_json, classes = [], {}
    for position, (definition, group, component) in enumerate(zip(definitions, groups, inputs)):
        source_id = "Source" + str(position + 1)
        clock = "RFModelClock" if capture["locked"] else ""
        if (definition.source_id != source_id or definition.reference_clock != clock or
                component.coherence_group != group):
            raise ValueError("Native source definition mapping changed")
        definitions_json.append(dict(source_id=source_id, reference_clock=clock, native_group=group,
            nominal_power_dbm=capture["source_powers_dbm"][position],
            nominal_phase_deg=capture["source_phases_deg"][position],
            native_input=_component_json(component)))
        key = _component_key(component)
        entry = classes.setdefault(key, dict(source_ids=[], phasor=0j))
        entry["source_ids"].append(source_id)
        entry["phasor"] += component.amplitude
    reduced = tuple(predicted.inputs) if predicted is not None else ()
    if predicted is not None and ({_component_key(c) for c in reduced} != set(classes) or
                                  len(reduced) != len(classes)):
        raise ValueError("Incomplete native reduced-input equivalence classes")
    order = [_component_key(c) for c in reduced] if predicted is not None else sorted(classes)
    result = []
    for position, key in enumerate(order):
        entry = classes[key]
        candidates = [d for d in direct if d["bin"] == key[0] and groups[d["source"] - 1] == key[3]]
        if len(candidates) != (0 if capture["diagnostic"] else 1):
            raise ValueError("Ambiguous observed source representative")
        representative = "Source" + str(candidates[0]["source"]) if candidates else None
        if representative is not None and representative not in entry["source_ids"]:
            raise ValueError("Observed representative is outside its source equivalence class")
        if predicted is not None:
            wave = reduced[position].amplitude
            scale = max(abs(entry["phasor"]), abs(wave), 1e-300)
            if abs(wave - entry["phasor"]) > 1e-13 * scale:
                raise ValueError("Reduced input omitted or changed a source phasor")
        result.append(dict(reduced_input_index=position + 1 if predicted is not None else None,
            key=dict(bin=key[0], kind="SOURCE", bandwidth_hz=key[2], native_group=key[3]),
            contributing_source_ids=entry["source_ids"],
            nominal_phasor_sum_sqrt_w=[entry["phasor"].real, entry["phasor"].imag],
            native_reduced_input=_component_json(reduced[position]) if predicted is not None else None,
            observed_representative=representative,
            representative_scope="observed alias of source equivalence class" if representative else
                                 "no deterministic output observation; diagnostic exclusion"))
    return dict(definitions=definitions_json, equivalence_classes=result,
                source_bandwidth_parameter_hz=1e6, actual_cw_bandwidth_hz=1.,
                spacing_hz=call["spacing_hz"], native_parameters=call["parameters"],
                diagnostic_only=capture["diagnostic"])


def _validate_source_mapping(capture, mapping, predicted, coherent):
    """Bind the mapper to exact archived origins, independently of magnitudes.

    In particular, Source2 is the observed alias in capture 15. Replacing it by
    Source1 must fail even though both names belong to the same physical class.
    """
    entries = mapping["equivalence_classes"]
    if [e["reduced_input_index"] for e in entries] != list(range(1, len(predicted.inputs) + 1)):
        raise ValueError("Missing or duplicated reduced source mapping")
    definitions = mapping["definitions"]
    if [d["source_id"] for d in definitions] != ["Source1", "Source2"]:
        raise ValueError("Incomplete original source definitions in mapping")
    key_fields = ("bin", "kind", "bandwidth_hz", "native_group")
    used_sources = []
    for entry, component in zip(entries, predicted.inputs):
        actual = _component_json(component)
        key = {field: actual[field] for field in key_fields}
        if entry["key"] != key or entry["native_reduced_input"] != actual:
            raise ValueError("Mapped reduced-input key/value disagrees with native input")
        members = [d["source_id"] for d in definitions
                   if {field: d["native_input"][field] for field in key_fields} == key]
        if not members or entry["contributing_source_ids"] != members:
            raise ValueError("Mapped contributing source membership changed")
        used_sources.extend(members)
    if sorted(used_sources) != ["Source1", "Source2"]:
        raise ValueError("Incomplete or duplicate source equivalence membership")
    observed = coherent.shared.two.spectrum_observer(capture)
    origins = []
    for term in predicted.terms:
        contributors, factors = [], []
        for index in term.input_indices:
            entry = entries[abs(index) - 1]
            source = entry["observed_representative"]
            if source not in ("Source1", "Source2") or source not in entry["contributing_source_ids"]:
                raise ValueError("Invalid observed alias mapping")
            sign = 1 if index > 0 else -1
            contributors.append(sign * (9 + int(source[-1])))
            # Keep signed factors with their full multiplicity; +x,+x,-x is not +x.
            factors.append(dict(sign=sign, source_equivalence_class=entry["contributing_source_ids"]))
        expression = coherent.traced.expression(contributors)
        _, label, bounds = observed(expression, term.component.bin, term.order, direct=term.order == 1)
        origins.append(dict(identity=label, bin=term.component.bin, kind=term.component.kind.name,
            bandwidth_hz=term.component.bandwidth_hz, frequency_bounds_hz=bounds, order=term.order,
            input_indices=list(term.input_indices), signed_source_equivalence_factors=factors))
    labels = [o["identity"] for o in origins]
    if len(set(labels)) != len(labels):
        raise ValueError("Duplicate mapped origin identity")
    return origins


def _json_key(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _check_identity(kind, check, capture):
    if kind == "shared":
        if check["measurement"] == "RFPwrIn(RFAmp)":
            return {"measurement": "RFPwrIn(RFAmp)"}
        if check["measurement"] != "direct_carrier":
            raise ValueError("Unknown shared measurement")
        source, index = check["source_label"], check["bin"]
        origin = f"D[(Source.Source{source})],Source,RFAmp"
        # Historical direct rows store a source number rather than IDName. Bind
        # both columns to the actual active archive label, including its vendor
        # group prefix, instead of inventing an identity from a native group.
        vectors = {}
        for name in ("IDNo", "IDName", "ID2"):
            matches = [n for n in capture["nodes"]
                       if n["path"].endswith("/System1_Data/Eqns/VarBlock/" + name)]
            if len(matches) != 1:
                raise ValueError("Missing/ambiguous archived identity vector")
            vectors[name] = matches[0]["data"]
        active = set(vectors["ID2"])
        labels = [label for number, label in zip(vectors["IDNo"], vectors["IDName"])
                  if number in active and label.startswith("{") and label.endswith("}" + origin)]
        if len(labels) != 1:
            raise ValueError("Missing/ambiguous full shared carrier identity")
        return dict(measurement="direct_carrier", source_label=source, bin=index, kind="SOURCE",
                    bandwidth_hz=1., frequency_bounds_hz=[index * 1e8 - .5, index * 1e8 + .5],
                    origin=origin, identity=labels[0])
    return {key: check[key] for key in ("order", "input_indices", "bin", "kind", "bandwidth_hz", "identity")}


def _index_checks(kind, report, captures):
    by_raw = {capture["raw_capture_sha256"]: (index + 1, capture) for index, capture in enumerate(captures)}
    result, seen_cases = {}, set()
    if len(report["reports"]) != 21 or len(report["diagnostics"]) != 2:
        raise ValueError("Incomplete scored/diagnostic capture coverage")
    for case in report["reports"]:
        raw = case["raw_capture_sha256"]
        if raw not in by_raw or raw in seen_cases:
            raise ValueError("Unknown or duplicate scored capture")
        seen_cases.add(raw)
        index, capture = by_raw[raw]
        if capture["diagnostic"] or any(case[key] != capture[key] for key in CASE_FIELDS):
            raise ValueError("Historical/current case configuration changed")
        expected = (2 if capture["locked"] else 3) if kind == "shared" else (
            4 if capture["locked"] else 15 if capture["same_frequency"] else 16)
        if len(case["checks"]) != expected:
            raise ValueError("Incomplete check collection for archived case")
        for check in case["checks"]:
            identity = _check_identity(kind, check, capture)
            key = (raw, _json_key(identity))
            if key in result:
                raise ValueError("Duplicate full measurement identity")
            result[key] = dict(case_id=f"shared-{index:02d}", capture_index=index,
                               raw_capture_sha256=raw, identity=identity, check=check)
    diagnostic_raw = [d["raw_capture_sha256"] for d in report["diagnostics"]]
    expected_raw = {c["raw_capture_sha256"] for c in captures if c["diagnostic"]}
    if len(set(diagnostic_raw)) != 2 or set(diagnostic_raw) != expected_raw:
        raise ValueError("Diagnostic identities changed")
    if any(d["classification"] != "diagnostic_only" or d["affects_compatibility_verdict"] is not False
           for d in report["diagnostics"]):
        raise ValueError("Diagnostic exclusion changed")
    return result


def _numeric_delta(now, before):
    if type(now) in (int, float) and type(before) in (int, float):
        return now - before
    if isinstance(now, list) and isinstance(before, list) and len(now) == len(before):
        return [_numeric_delta(a, b) for a, b in zip(now, before)]
    return None


def _delta(now, before):
    changed = sorted(key for key in set(now) | set(before) if now.get(key) != before.get(key))
    numeric = {key: _numeric_delta(now.get(key), before.get(key)) for key in changed}
    return dict(changed_fields=changed, numeric={k: v for k, v in numeric.items() if v is not None},
                status_changed=now.get("passed") != before.get("passed"))


def _comparison(kind, current, historical, captures):
    if current["relative_tolerance"] != 1e-7 or historical["relative_tolerance"] != 1e-7:
        raise ValueError("Historical vendor tolerance changed")
    if kind == "shared" and (current["cancellation_absolute_tolerance_w"] != 1e-15 or
                             historical["cancellation_absolute_tolerance_w"] != 1e-15):
        raise ValueError("Historical absolute tolerance changed")
    now, before = (_index_checks(kind, r, captures) for r in (current, historical))
    if set(now) != set(before):
        raise ValueError("Historical/current full measurement identities changed")
    rows = []
    for key, item in now.items():
        a, b = item["check"], before[key]["check"]
        observed = ("systemvue_w", "tolerance_w") if kind == "shared" and "systemvue_w" in a else (
            ("systemvue_boundary_waves",) if kind == "shared" else
            ("systemvue_amplitude", "systemvue_power_w"))
        if any(a[field] != b[field] for field in observed):
            raise ValueError("Historical/current vendor observation or threshold changed")
        rows.append({k: v for k, v in item.items() if k != "check"} |
                    dict(category=kind, current=a, historical=b, delta=_delta(a, b)))
    count = 53 if kind == "shared" else 206
    if len(rows) != count:
        raise ValueError("Incomplete archived measurement identities")
    return dict(rows=rows, differences=[r for r in rows if r["delta"]["changed_fields"]],
                numeric_differences=[r for r in rows if r["delta"]["numeric"]],
                status_differences=[r for r in rows if r["delta"]["status_changed"]])


def _partition(case):
    # Native numeric groups are local labels, never source IDs or cross-build keys.
    classes = {}
    for check in case["checks"]:
        classes.setdefault(check["native_group"], []).append(check["identity"])
    return sorted(sorted(labels) for labels in classes.values())


def replay(probe, *, root=ROOT):
    forbid_product_imports()
    paths = {name: Path(root) / "validation" / name for name in ARCHIVE_SHA256}
    raw = {name: path.read_bytes() for name, path in paths.items()}
    if {name: _sha256(value) for name, value in raw.items()} != ARCHIVE_SHA256:
        raise ValueError("Frozen source archive bytes changed")
    if {name: _sha256((HERE / name).read_bytes()) for name in FROZEN_SHA256} != FROZEN_SHA256:
        raise ValueError("Frozen source checker/support bytes changed")
    data = {name: json.loads(value.decode("utf-8-sig")) for name, value in raw.items()}
    captures = data[CAPTURES]
    _validate_cases(captures)
    historical = {kind: data[name] for kind, name in HISTORY.items()}
    crlf_sha256 = _sha256(raw[CAPTURES].replace(b"\n", b"\r\n"))
    if b"\r\n" in raw[CAPTURES] or any(h["captures_sha256"] != crlf_sha256 for h in historical.values()):
        raise ValueError("Historical CRLF capture hash relation changed")
    provider = NativeSourceReplay(probe)
    recorder = _RecordingProvider(provider)
    shared = _load_checker("compare-shared-compression.py")
    coherent = _load_checker("compare-coherent-amplifier.py")
    reports = dict(shared=shared.compare(recorder, captures), coherent=coherent.compare(recorder, captures))
    if len(recorder.shared_calls) != 23 or len(recorder.coherent_calls) != 21:
        raise NativeReplayError("Incomplete native archive requests")
    comparison = {kind: _comparison(kind, reports[kind], historical[kind], captures) for kind in HISTORY}
    coherent_calls = iter(recorder.coherent_calls)
    mappings = []
    for index, (capture, shared_call) in enumerate(zip(captures, recorder.shared_calls), 1):
        call = None if capture["diagnostic"] else next(coherent_calls)
        predicted = call["result"] if call is not None else None
        mapping = _source_mapping(capture, call or shared_call, shared, predicted)
        mapping.update(case_id=f"shared-{index:02d}", capture_index=index,
                       raw_capture_sha256=capture["raw_capture_sha256"])
        mapping["origins"] = [] if predicted is None else _validate_source_mapping(capture, mapping, predicted, coherent)
        mappings.append(mapping)
    partitions = []
    old_by_raw = {r["raw_capture_sha256"]: r for r in historical["coherent"]["reports"]}
    case_ids = {m["raw_capture_sha256"]: m["case_id"] for m in mappings}
    for current in reports["coherent"]["reports"]:
        previous = old_by_raw[current["raw_capture_sha256"]]
        a = dict(passed=current["group_partition_passed"], equivalence_classes=_partition(current))
        b = dict(passed=previous["group_partition_passed"], equivalence_classes=_partition(previous))
        partitions.append(dict(case_id=case_ids[current["raw_capture_sha256"]],
            raw_capture_sha256=current["raw_capture_sha256"], identity="group_partition",
            current=a, historical=b, delta=_delta(a, b)))
    summary = {}
    for kind in HISTORY:
        rows = comparison[kind]["rows"]
        passed = sum(row["current"]["passed"] for row in rows)
        old_passed = sum(row["historical"]["passed"] for row in rows)
        summary[kind] = dict(check_count=len(rows), passed_count=passed, failed_count=len(rows) - passed,
            vendor_passed=reports[kind]["passed"], historical_passed_count=old_passed,
            historical_failed_count=len(rows) - old_passed, historical_vendor_passed=historical[kind]["passed"])
    summary.update(capture_count=23, scored_case_count=21, excluded_diagnostic_count=2,
        direct_boundary_complex_comparison_count=sum(len(r["current"].get("relative_complex_errors", []))
                                                    for r in comparison["shared"]["rows"]),
        group_partition_check_count=len(partitions), group_partition_passed_count=sum(r["current"]["passed"] for r in partitions))
    claims = dict(available_capture_bytes_sha256=_sha256(raw[CAPTURES]),
        historical_capture_sha256=crlf_sha256, reconstructed_crlf_sha256=crlf_sha256,
        crlf_relation_verified=True,
        crlf_explanation="Only actual LF bytes are converted to CRLF in memory; escaped script text is untouched. This verifies a newline relation, not possession of historical original bytes.",
        raw_capture_claims=[dict(case_id=f"shared-{i:02d}", sha256=c["raw_capture_sha256"],
            verification="historical_declared_unverified") for i, c in enumerate(captures, 1)],
        workspace_claims=[dict(sha256=digest, scope=scope, verification="historical_declared_unverified")
            for digest, scope in sorted({(c["source_workspace_sha256"], c["workspace_hash_scope"]) for c in captures})],
        historical_library_claims=[dict(historical_report=HISTORY[kind], sha256=history["library_sha256"],
            verification="historical_declared_unverified") for kind, history in historical.items()],
        original_raw_verified=False, original_workspace_verified=False, original_library_verified=False)
    manifest = provider.manifest(paths.values(), sorted(HERE.glob("*.py")))
    if manifest["available_archive_bytes_sha256"] != {str(paths[n]): _sha256(b) for n, b in raw.items()}:
        raise NativeReplayError("Archived bytes changed during replay")
    if {name: _sha256((HERE / name).read_bytes()) for name in FROZEN_SHA256} != FROZEN_SHA256:
        raise NativeReplayError("Frozen source checker/support bytes changed during replay")
    return dict(schema="rfmodel.source-archives-replay", schema_version=1,
        recipe_version="shared-source-cubic-v1", scorer_version="frozen-shared-and-coherent-v1",
        source_mapping_version="source-equivalence-observed-alias-v1",
        classification="historical_archive_replay", new_systemvue_analysis_calls=0,
        eligible_for_full_model_compatibility=False,
        scope="23 existing shared captures, two distinct unchanged judgments: 53 shared drive/direct checks; 206 single-stage complex-and-power origins plus 21 group partitions",
        reports=reports, historical_reports=historical, comparison=comparison, group_partitions=partitions,
        source_equivalence_mapping=mappings, summary=summary, archive_identity=claims,
        native_provenance=manifest,
        diagnostic_exclusions=[dict(case_id=m["case_id"], raw_capture_sha256=m["raw_capture_sha256"],
            classification="diagnostic_only", affects_compatibility_verdict=False,
            observation_state="spectrum_present_without_deterministic_carriers" if captures[m["capture_index"]-1]["noise_enabled"] else "all_five_output_vectors_absent",
            measured_zero=False) for m in mappings if m["diagnostic_only"]],
        limitations=["No new SystemVue run; original raw, workspace and historical library bytes are unverified",
            "Shared archives retain component names, controlled scripts and SI readbacks, not native Model/Netlist or numeric Unit IDs",
            "Coherent origins do not score RFPwrIn; their pass does not override the shared drive verdict",
            "No noise-drive fit, NodeTotal, saturation/reverse-drive acceptance, cascade or secondary mixing",
            "Observed merged-source representative is an alias, not exclusive physical ownership of the term",
            "The fixed matrix contains no locked different-frequency vendor case"],
        invocation=dict(executable=sys.executable, argv=list(sys.argv), cwd=str(Path.cwd())),
        interpretation="Current predictions and pass flags are computed by the native probe and unchanged scorers. Historical numbers are comparison columns only. A successful replay gate may retain vendor failures; all actual differences remain visible.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-probe", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = replay(args.native_probe)
    atomic_write_report(args.report, report, protected_paths=[args.native_probe,
        str(args.native_probe) + ".manifest.json", *[ROOT / "validation" / n for n in ARCHIVE_SHA256], *HERE.glob("*.py")])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
