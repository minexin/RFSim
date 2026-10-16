"""Versioned diagnostic replay of eight immutable cubic source-cascade archives.

Raw stage paths and typed physical outputs have independent observed oracles.
Historical predictions are comparison columns only. No retired graph is invoked.
"""
import argparse
import cmath
import importlib.util
import json
import math
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from native_replay import NativeReplayError, _sha256, atomic_write_report, forbid_product_imports
from reference_records import CoherentComponent, SpectrumKind

CAPTURES = "systemvue-2023-cascade-captures.json"
HISTORY = "systemvue-2023-cascade-amplifier.json"
ARCHIVE_SHA256 = {
    CAPTURES: "4e806080f6e4576edf244ab69e4f6f962d785eb5dba0944539a5edc1aae3fa0a",
    HISTORY: "215b2cfd1f963a83da671f16b629200c05bdf94b8fb942c2bfe5b60bf831b206",
}
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
CASE_CONFIGURATIONS = (
    (-20, 90, 3, 0, 100, 1e6, False, False, -50),
    (-20, 90, 3, 0, 140, 1e6, False, False, -50),
    (-10, 45, 3, 10, 100, 1e6, False, False, -50),
    (-10, 45, 3, 10, 100, 1e6, False, True, -140),
    (-30, 0, 3, 10, 100, 1, False, False, -50),
    (-30, 0, 3, 10, 100, 1e6, False, False, -50),
    (-30, 90, 3, 10, 100, 1e6, True, False, -50),
    (-10, 90, 3, 10, 100, 1e6, True, True, -140),
)
ASSUMPTION = dict(assumed_value=True, assumption_source="legacy_checker_default",
                  vendor_readback_verified=False, eligibility="diagnostic_only")


def _checker():
    spec = importlib.util.spec_from_file_location("cascade_archive_frozen", HERE / "compare-cascade-amplifier.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def _json_key(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _key(c):
    return c.bin, int(c.kind), c.bandwidth_hz, c.coherence_group


def _canonical(factors):
    return tuple(sorted(factors))  # Preserve repetitions and opposite signs.


def _expression(checker, factors):
    return checker.traced.expression(tuple((1 if i > 0 else -1) * (9 + abs(i)) for i in factors))


def _physical(c, factors):
    return c.bin, int(c.kind), c.bandwidth_hz, _canonical(factors)


def _validate_cases(captures, checker):
    if not isinstance(captures, list) or len(captures) != 8:
        raise ValueError("Expected exactly eight cascade captures")
    if tuple(tuple(c[k] for k in checker.META) for c in captures) != CASE_CONFIGURATIONS:
        raise ValueError("Cascade configuration coverage changed")
    if len({c["raw_capture_sha256"] for c in captures}) != 8:
        raise ValueError("Duplicate cascade raw identity")
    for capture in captures:
        if any(k in capture for k in ("spectrum_reduction", "first_op1db_dbm", "second_source_power_dbm")):
            raise ValueError("Historical default evidence changed")
        if any(n["path"].endswith(("/UseSpecReduction", "/ElimSpec", "/IgnorePwrLvl")) for n in capture["nodes"]):
            raise ValueError("Unexpected historical spectrum-reduction readback")


def _joint(predicted, wave):
    if not abs(wave) or not all(math.isfinite(x) for x in (predicted.real, predicted.imag, wave.real, wave.imag)):
        raise ValueError("Nonfinite or zero observed RF wave")
    error = abs(predicted - wave) / abs(wave)
    power_error = abs(predicted) ** 2 / abs(wave) ** 2 - 1
    return dict(relative_complex_error=error, relative_power_error=power_error,
                passed=error <= 1e-7 and abs(power_error) <= 1e-7)


def compare(provider, captures, checker=None):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty capture list required")
    checker = checker or _checker()
    reports, mappings, unscored = [], [], []
    seen = set()
    for index, capture in enumerate(captures, 1):
        tones, parameters = checker.inspect(capture)
        configuration = tuple(capture[k] for k in checker.META)
        if configuration in seen:
            raise ValueError("Duplicate configuration")
        seen.add(configuration)
        if capture["maximum_order"] != 3 or capture.get("spectrum_reduction", True) is not True:
            raise ValueError("Only the fixed cubic diagnostic recipe is supported")
        if capture.get("second_source_power_dbm", capture["source_power_dbm"]) != capture["source_power_dbm"]:
            raise ValueError("Only equal nominal source powers are supported")
        sources = tuple(CoherentComponent(10+i, SpectrumKind.SOURCE, 1., i+1,
            cmath.rect(math.sqrt(10 ** ((capture["source_power_dbm"]-30)/10)),
                       math.radians(capture["source_phase_deg"] if tones == 1 or i == 1 else 0)))
            for i in range(tones))
        result = provider.two_stage(1e8, sources, second_gain_db=capture["second_gain_db"])
        first, second = result.first, result.second
        if tuple(first.inputs) != sources:
            raise ValueError("Native input sources changed")
        first_factors = {}
        for term in first.terms:
            factors = _canonical(term.input_indices)
            if _key(term.component) in first_factors:
                raise ValueError("Duplicate first-stage physical input")
            if any(type(i) is not int or not 1 <= abs(i) <= tones for i in factors):
                raise ValueError("Unknown primitive source")
            first_factors[_key(term.component)] = factors
        if set(map(_key, second.inputs)) != set(first_factors) or len(second.inputs) != len(first_factors):
            raise ValueError("Incomplete conducted stage inputs")
        checks, grouped, measured_groups, mapped = [], {}, {}, []
        for stage, response, port in ((1, first, 3), (2, second, 2)):
            observed, used = checker.spectrum(capture, port), set()
            for term in response.terms:
                component = term.component
                if stage == 1:
                    factors = first_factors[_key(component)]
                elif term.order == 1:
                    if len(term.input_indices) != 1 or term.input_indices[0] <= 0:
                        raise ValueError("Invalid conducted index")
                    parent = second.inputs[term.input_indices[0]-1]
                    factors = first_factors[_key(parent)]
                    if (component.bin, component.kind, component.bandwidth_hz) != (parent.bin, parent.kind, parent.bandwidth_hz):
                        raise ValueError("Conducted component role changed")
                else:
                    values = []
                    for i in term.input_indices:
                        parent = second.inputs[abs(i)-1]
                        primitive = first_factors[_key(parent)]
                        if parent.kind != SpectrumKind.SOURCE or len(primitive) != 1:
                            raise ValueError("Secondary distortion generation is outside recipe")
                        values.append((1 if i > 0 else -1) * primitive[0])
                    factors = _canonical(values)
                if component.bin != sum((1 if i > 0 else -1) * sources[abs(i)-1].bin for i in factors) or component.bandwidth_hz != len(factors):
                    raise ValueError("Primitive frequency or bandwidth support changed")
                expression = _expression(checker, factors)
                identity = checker.label(expression, stage, direct=term.order == 1 and component.kind == SpectrumKind.SOURCE,
                                         conducted=term.order == 1)
                if identity in used:
                    raise ValueError("Duplicate native origin")
                used.add(identity)
                rows = observed.get(identity, [])
                bounds = [component.bin*1e8-component.bandwidth_hz/2, component.bin*1e8+component.bandwidth_hz/2]
                if len(rows) != 2 or [r[0] for r in rows] != bounds or rows[0][2] != rows[1][2]:
                    raise ValueError("Missing or malformed origin spectrum: " + identity)
                scale = math.sqrt(min(1., capture["channel_bandwidth_hz"] / component.bandwidth_hz))
                wave = rows[0][1] / scale
                if abs(rows[1][1]/scale-wave) > 1e-12*abs(wave):
                    raise ValueError("Non-flat CW spectrum")
                checks.append(dict(stage=stage, identity=identity, bandwidth_hz=component.bandwidth_hz,
                    systemvue_wave=[wave.real, wave.imag], rfmodel_wave=[component.amplitude.real, component.amplitude.imag],
                    **_joint(component.amplitude, wave)))
                physical = _physical(component, factors)
                measured_groups.setdefault(physical, set()).add(rows[0][2])
                mapped.append(dict(stage=stage, identity=identity, bin=component.bin, kind=int(component.kind),
                    bandwidth_hz=component.bandwidth_hz, signed_source_factors=list(factors),
                    native_group=component.coherence_group, vendor_group=rows[0][2], frequency_bounds_hz=bounds,
                    measured_channel_fraction=scale**2))
                if stage == 2:
                    grouped[physical] = grouped.get(physical, 0j) + wave
            expected = {identity for identity in observed if stage == 2 or identity.endswith(",RFAmp1")}
            if used != expected:
                raise ValueError("Incomplete native RF spectrum")
            if stage == 1:
                for identity in sorted(set(observed)-used):
                    unscored.append(dict(case_id=f"cascade-{index:02d}", port=3, identity=identity,
                        boundary_records=[dict(frequency_hz=f, wave_sqrt_w=[w.real,w.imag], vendor_group=g) for f,w,g in observed[identity]],
                        classification="unscored_origin", direction="unknown", measured_zero=False,
                        reason="Outside selected stage1 origin family; no independent direction evidence"))
        if any(len(g) != 1 for g in measured_groups.values()) or len({next(iter(g)) for g in measured_groups.values()}) != len(measured_groups):
            raise ValueError("Measured physical-origin group partition changed")
        combined = {}
        for record in result.combined:
            physical = _physical(record.component, record.factors)
            if physical in combined:
                raise ValueError("Duplicate typed combined identity")
            combined[physical] = record.component.amplitude
        if set(combined) != set(grouped):
            raise ValueError("Typed combined primitive identity coverage changed")
        for physical, wave in grouped.items():
            bin_, kind, bandwidth, factors = physical
            predicted = combined[physical]
            checks.append(dict(stage="combined", origin=[_expression(checker, factors), bin_, kind, bandwidth],
                systemvue_wave=[wave.real,wave.imag], rfmodel_wave=[predicted.real,predicted.imag],
                signed_source_factors=list(factors), **_joint(predicted,wave)))
        for stage, response in enumerate((first, second), 1):
            measured = checker.vector(capture,"RFPwrIn")[stage-1]
            predicted = response.total_input_power_w
            if not math.isfinite(predicted) or predicted < 0 or measured <= 0:
                raise ValueError("Invalid native/observed drive")
            error = abs(predicted/measured-1)
            checks.append(dict(stage=stage,measurement="RFPwrIn",systemvue_w=measured,rfmodel_w=predicted,
                               relative_error=error,passed=error<=1e-7))
        reports.append(dict(configuration={k:capture[k] for k in checker.META},
            raw_capture_sha256=capture["raw_capture_sha256"], parameters=parameters, checks=checks,
            spectrum_reduction=dict(ASSUMPTION), eligibility="diagnostic_only", passed=all(c["passed"] for c in checks)))
        mappings.append(dict(case_id=f"cascade-{index:02d}",origins=mapped,
            physical_roots=[dict(r._asdict()) for r in result.roots],
            typed_stage_statistics=[dict(r._asdict()) for r in result.stage_stats],
            source_definitions=[dict(source_id=f"Source{i+1}",bin=c.bin,bandwidth_hz=c.bandwidth_hz,
                native_group=c.coherence_group,amplitude_sqrt_w=[c.amplitude.real,c.amplitude.imag],reference_clock="")
                for i,c in enumerate(sources)]))
    return dict(relative_tolerance=1e-7,reports=reports,passed=all(r["passed"] for r in reports),
        eligibility="diagnostic_only", full_port_spectrum_verified=False,
        stage1_scope="selected origin family ending ,RFAmp1 only",
        unscored_port3_origin_count=len(unscored)), mappings, unscored


def _identity(check):
    if "measurement" in check:
        return {k:check[k] for k in ("stage","measurement")}
    if check["stage"] == "combined":
        return {k:check[k] for k in ("stage","origin")}
    return {k:check[k] for k in ("stage","identity","bandwidth_hz")}


def _index(report, captures):
    expected = {c["raw_capture_sha256"]:(i,c) for i,c in enumerate(captures,1)}
    indexed, seen = {}, set()
    checker = _checker()
    for case in report["reports"]:
        raw = case["raw_capture_sha256"]
        if raw not in expected or raw in seen:
            raise ValueError("Unknown or duplicate scored capture")
        seen.add(raw)
        i,capture = expected[raw]
        if case["configuration"] != {k:capture[k] for k in checker.META}:
            raise ValueError("Case configuration changed")
        if len(case["checks"]) != (64 if capture["two_tone"] else 17):
            raise ValueError("Incomplete check collection")
        for check in case["checks"]:
            identity = _identity(check)
            key = raw,_json_key(identity)
            if key in indexed:
                raise ValueError("Duplicate measurement identity")
            indexed[key] = dict(case_id=f"cascade-{i:02d}",capture_index=i,raw_capture_sha256=raw,identity=identity,check=check)
    if seen != set(expected):
        raise ValueError("Missing scored capture")
    return indexed


def _delta(now,before):
    changed = sorted(k for k in set(now)|set(before) if now.get(k)!=before.get(k))
    def numeric(a,b):
        if type(a) in (int,float) and type(b) in (int,float): return a-b
        if isinstance(a,list) and isinstance(b,list) and len(a)==len(b): return [numeric(x,y) for x,y in zip(a,b)]
        return None
    values = {k:numeric(now[k],before[k]) for k in changed if k in now and k in before}
    return dict(changed_fields=changed,numeric={k:v for k,v in values.items() if v is not None},status_changed=now["passed"]!=before["passed"])


def _comparison(current,historical,captures):
    if current["relative_tolerance"] != 1e-7 or historical["relative_tolerance"] != 1e-7:
        raise ValueError("Historical tolerance changed")
    now,before = (_index(r,captures) for r in (current,historical))
    if set(now)!=set(before): raise ValueError("Full measurement identity coverage changed")
    rows=[]
    for key,item in now.items():
        a,b=item["check"],before[key]["check"]
        for field in ("systemvue_w","systemvue_wave"):
            if field in b and a.get(field)!=b[field]: raise ValueError("Vendor observation changed")
        rows.append({k:v for k,v in item.items() if k!="check"} | dict(current=a,historical=b,delta=_delta(a,b)))
    if len(rows)!=230: raise ValueError("Incomplete cascade measurement identities")
    return dict(rows=rows,differences=[r for r in rows if r["delta"]["changed_fields"]],
                numeric_differences=[r for r in rows if r["delta"]["numeric"]],
                status_differences=[r for r in rows if r["delta"]["status_changed"]])


def _frozen():
    if {n:_sha256((HERE/n).read_bytes()) for n in FROZEN_SHA256} != FROZEN_SHA256:
        raise ValueError("Frozen cascade checker/support bytes changed")


def replay(probe, *, root=ROOT):
    forbid_product_imports()
    _frozen()
    paths={n:Path(root)/"validation"/n for n in ARCHIVE_SHA256}
    raw={n:p.read_bytes() for n,p in paths.items()}
    if {n:_sha256(b) for n,b in raw.items()} != ARCHIVE_SHA256:
        raise ValueError("Frozen cascade archive bytes changed")
    captures,historical=(json.loads(raw[n].decode("utf-8-sig")) for n in (CAPTURES,HISTORY))
    checker=_checker()
    _validate_cases(captures,checker)
    crlf=_sha256(raw[CAPTURES].replace(b"\n",b"\r\n"))
    if b"\r\n" in raw[CAPTURES] or crlf!=historical["captures_sha256"]:
        raise ValueError("Historical CRLF relation changed")
    from native_cascade_replay import NativeCascadeReplay
    provider=NativeCascadeReplay(probe)
    current,mappings,unscored=compare(provider,captures,checker)
    comparison=_comparison(current,historical,captures)
    rows=comparison["rows"]
    passed=sum(r["current"]["passed"] for r in rows)
    old_passed=sum(r["historical"]["passed"] for r in rows)
    summary=dict(capture_count=8,check_count=len(rows),passed_count=passed,failed_count=len(rows)-passed,
        historical_passed_count=old_passed,historical_failed_count=len(rows)-old_passed,vendor_passed=current["passed"],
        raw_path_check_count=sum("identity" in r["identity"] for r in rows),
        combined_check_count=sum(r["identity"]["stage"]=="combined" for r in rows),
        drive_check_count=sum("measurement" in r["identity"] for r in rows),
        power_only_failed_count=sum(not r["current"]["passed"] and r["current"].get("relative_complex_error",math.inf)<=1e-7 for r in rows),
        unscored_port3_origin_count=len(unscored), full_port_spectrum_verified=False,
        common_numeric_difference_row_count=len(comparison["numeric_differences"]),
        common_numeric_difference_field_count=sum(len(r["delta"]["numeric"]) for r in rows),
        status_difference_count=len(comparison["status_differences"]))
    claims=dict(available_capture_bytes_sha256=_sha256(raw[CAPTURES]),historical_capture_sha256=historical["captures_sha256"],
        reconstructed_crlf_sha256=crlf,crlf_relation_verified=True,
        crlf_explanation="LF to CRLF in-memory relation only; historical original bytes are unavailable",
        raw_capture_claims=[dict(case_id=f"cascade-{i:02d}",sha256=c["raw_capture_sha256"],verification="historical_declared_unverified") for i,c in enumerate(captures,1)],
        workspace_claims=[dict(sha256=d,scope="not_declared_in_archive; collector hashes on-disk file, not live memory snapshot",verification="historical_declared_unverified") for d in sorted({c["workspace_sha256"] for c in captures})],
        historical_library_claims=[dict(sha256=historical["library_sha256"],verification="historical_declared_unverified")],
        original_raw_verified=False,original_workspace_verified=False,original_library_verified=False)
    manifest=provider.manifest(paths.values(),sorted(HERE.glob("*.py")))
    if manifest["available_archive_bytes_sha256"]!={str(paths[n]):_sha256(b) for n,b in raw.items()}:
        raise NativeReplayError("Archived bytes changed during replay")
    _frozen()
    frozen_identity = {name: dict(available_bytes_sha256=_sha256((HERE/name).read_bytes()),
        canonical_lf_sha256=_sha256((HERE/name).read_bytes().replace(b"\r\n", b"\n")),
        expected_frozen_lf_sha256=digest) for name,digest in FROZEN_SHA256.items()}
    if any(row["canonical_lf_sha256"] != row["expected_frozen_lf_sha256"] for row in frozen_identity.values()):
        raise NativeReplayError("Frozen canonical LF source identity changed")
    evidence = [dict(case_id=f"cascade-{i:02d}",
        spectrum_reduction=dict(ASSUMPTION), numeric_ui_unit_ids_verified=False,
        parameter_evidence=[dict(path=n["path"], data_entry=n["data_entry"], si_readback=n["data"],
                                unit_attribute_declared="Unit:Long" in n.get("variables", []))
            for n in c["nodes"] if n.get("data_entry") is not None],
        dataset_timestamps=[dict(path=n["path"],timestamp=n["timestamp"]) for n in c["nodes"]
            if n["path"].endswith(("/System3_Design3_Data", "/System3_Design3_Data_Path1"))],
        default_parameters=dict(first_op1db_dbm=20, second_source_power_dbm=c["source_power_dbm"],
                                verified_against_si_readbacks=True)) for i,c in enumerate(captures,1)]
    return dict(schema="rfmodel.cascade-archives-replay",schema_version=1,recipe_version="cubic-source-cascade-v1",
        scorer_version="typed-cascade-path-and-physical-v1",classification="historical_archive_replay",
        eligibility="diagnostic_only",eligible_for_full_model_compatibility=False,new_systemvue_analysis_calls=0,
        spectrum_reduction=dict(ASSUMPTION),reports=current,historical_reports=historical,comparison=comparison,
        summary=summary,source_identity_mapping=mappings,unscored_port3_origins=unscored,
        archive_identity=claims,native_provenance=manifest,
        frozen_source_identity=frozen_identity,capture_evidence=evidence,
        limitations=["Spectrum reduction was not read back in any of the eight archives; physical acceptance depending on it is blocked",
            "Combined vendor oracle is observed same-origin path complex-wave sum, not NodeTotal or raw-model sum",
            "Only selected stage1 origin family is scored; other port3 origins have unknown direction and are never assigned zero",
            "No secondary remixing, noise, AM/PM, higher-order, complete path-budget or reverse-drive verdict",
            "DataEntry/SI pairs are available but numeric UI Unit IDs are absent",
            "No new SystemVue run; historical raw/workspace/library bytes remain unverified"],
        invocation=dict(executable=sys.executable,argv=list(sys.argv),cwd=str(Path.cwd())))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-probe",type=Path,required=True)
    parser.add_argument("--report",type=Path,required=True)
    args=parser.parse_args()
    report=replay(args.native_probe)
    atomic_write_report(args.report,report,protected_paths=[args.native_probe,str(args.native_probe)+".manifest.json",
        *[ROOT/"validation"/n for n in ARCHIVE_SHA256],*HERE.glob("*.py")])
    return 0


if __name__=="__main__":
    raise SystemExit(main())
