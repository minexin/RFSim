"""Normalize A01 raw captures without COM; preserve report/raw disagreements."""

import argparse
from datetime import datetime
import hashlib
import importlib.util
import json
import math
import re
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "hypotheses", Path(__file__).with_name("analyze-am-pm-datapoints.py")
)
hypotheses = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hypotheses)
BASE = "RFModel_AMPM_A01/DataPoints/"
PATH = BASE + "System2_Data_Folder/System2_Sch2_Data_Path1"
RAW = BASE + "System2_Sch2_Data"
AMP = BASE + "Sch2/PartList/RFAmp/ParamSet/"
SOURCE = BASE + "Sch2/PartList/Source/ParamSet/"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def relative(a, b):
    return abs(a - b) / max(abs(a), abs(b), 1e-300)


def parse_utc(text):
    # Windows DateTime emits seven fractional digits; Python 3.9/3.10 accept only
    # three or six. Truncate only for second-level freshness checks, retain raw text.
    if not isinstance(text, str):
        raise ValueError("UTC timestamp must be text")
    match = re.fullmatch(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?(Z|\+00:00)", text)
    if match is None:
        raise ValueError("Expected an explicit UTC timestamp")
    fraction = (match.group(2) or "")[:6].ljust(6, "0")
    return datetime.fromisoformat(match.group(1) + "." + fraction + "+00:00").timestamp()


def normalize(capture, profile):
    nodes = {}
    for node in capture["nodes"]:
        nodes.setdefault(node["path"], []).append(node)

    def node(path):
        if path not in nodes or len(nodes[path]) != 1:
            raise ValueError("Missing or ambiguous required node: " + path)
        return nodes[path][0]

    def data(path):
        return node(path)["data"]

    def vector(path, count=None):
        item = node(path)
        values = item["data"]
        if not isinstance(values, list) or item["dimensions"] != [len(values)]:
            raise ValueError("Unexpected vector shape: " + path)
        if count is not None and len(values) != count:
            raise ValueError("Unexpected vector length: " + path)
        return values

    if capture.get("manager_errors", "").strip():
        raise ValueError("Diagnostic capture contains warnings/errors")
    start = parse_utc(capture["run_started_utc"])
    returned = capture.get("run_returned_utc")
    if returned is None and capture.get("analysis_done") != 1:
        raise ValueError("Recovered capture requires explicit completed-analysis observation")
    end_text = returned or capture["analysis_done_observed_utc"]
    end = parse_utc(end_text)
    if end < start:
        raise ValueError("Reversed time interval")
    timestamps = {key: int(node(key)["timestamp"]) for key in (PATH, RAW)}
    if any(not math.floor(start) <= stamp <= math.ceil(end) for stamp in timestamps.values()):
        raise ValueError("Stale dataset")
    key = PATH + "/Eqns/VarBlock/"
    if vector(key + "NodeNames", 2) != ["1", "2"] or vector(key + "CF", 2) != [1e8, 1e8]:
        raise ValueError("Unexpected node or frequency mapping")
    power = [hypotheses.number(x) for x in vector(key + "DCP", 2)]
    phase = [math.degrees(hypotheses.number(x)) for x in vector(key + "DCPH", 2)]
    if any(x <= 0 for x in power):
        raise ValueError("Nonpositive channel power")
    readbacks = {}
    expected = {
        "AMtoPM_Mode": [2],
        "G": [100],
        "OP1dB": [1e7],
        "OPSAT": [10**7.3],
        "OIP2": [1e11],
        "OIP3": [1e10],
        "PwrAMtoPMpoints": [10 ** ((x - 30) / 10) for x in profile["power_dbm"]],
        "AMtoPMpoints": [math.radians(x) for x in profile["phase_deg"]],
    }
    for name, wanted in expected.items():
        raw = data(AMP + name)
        values = raw if isinstance(raw, list) else [raw]
        if len(values) != len(wanted) or any(
            relative(hypotheses.number(a), b) > 1e-12 for a, b in zip(values, wanted)
        ):
            raise ValueError("Parameter unit/value mismatch: " + name)
        readbacks[name] = raw
    target = hypotheses.number(capture["target_output_dbm"])
    if target not in profile["output_targets_dbm"]:
        raise ValueError("Unplanned sample")
    expected_source = 10 ** ((target - 50) / 10)
    if relative(hypotheses.number(data(SOURCE + "Pwr")), expected_source) > 1e-12:
        raise ValueError("Source power readback mismatch")
    if hypotheses.circular_error(phase[0], profile["source_phase_deg"]) > 1e-5:
        raise ValueError("Source phase mismatch")
    if (
        abs(hypotheses.number(data(SOURCE + "Phase")) - math.radians(profile["source_phase_deg"]))
        > 1e-12
    ):
        raise ValueError("Source phase parameter mismatch")
    raw_key = RAW + "/Eqns/VarBlock/"
    names = vector(raw_key + "IDName")
    ids = vector(raw_key + "IDNo", len(names))
    mapping = dict(zip(ids, names))
    wanted_ids = [
        i
        for i, name in mapping.items()
        if name.startswith("{1}D[") and name.endswith(",Source,RFAmp")
    ]
    if len(wanted_ids) != 1:
        raise ValueError("Missing/ambiguous desired output signal")
    frequencies = vector(raw_key + "F2")
    count = len(frequencies)
    labels = vector(raw_key + "ID2", count)
    powers = vector(raw_key + "P2", count)
    volts = vector(raw_key + "V2", 2 * count)
    impedances = vector(raw_key + "Z2", 2 * count)
    carrier = []
    for i, label in enumerate(labels):
        if label != wanted_ids[0]:
            continue
        v = complex(*map(hypotheses.number, volts[2 * i : 2 * i + 2]))
        z = complex(*map(hypotheses.number, impedances[2 * i : 2 * i + 2]))
        p = hypotheses.number(powers[i])
        angle = math.degrees(math.atan2(v.imag, v.real))
        if z == 0 or p <= 0 or relative(0.5 * abs(v) ** 2 * (1 / z).real, p) > 1e-12:
            raise ValueError("Raw voltage/impedance/power inconsistency")
        if hypotheses.circular_error(angle, phase[1]) > 1e-8:
            raise ValueError("Raw and reported phase disagree")
        carrier.append(
            dict(
                frequency_hz=frequencies[i],
                power_w=p,
                voltage=[v.real, v.imag],
                impedance=[z.real, z.imag],
                phase_deg=angle,
                label=mapping[label],
            )
        )
    if len(carrier) != 2 or sorted(c["frequency_hz"] for c in carrier) != [99999999.5, 100000000.5]:
        raise ValueError("Unexpected single-tone spectral support")
    if relative(carrier[0]["power_w"], carrier[1]["power_w"]) > 1e-12:
        raise ValueError("Nonconstant single-tone spectrum")
    raw_power = sum(c["power_w"] for c in carrier) / 2
    error = relative(raw_power, power[1])
    return dict(
        profile=profile["name"],
        target_output_dbm=target,
        measured_output_w=power[1],
        measured_output_phase_deg=phase[1],
        measured_source_phase_deg=phase[0],
        raw_output_w=raw_power,
        reported_output_dbm=10 * math.log10(power[1]) + 30,
        raw_output_dbm=10 * math.log10(raw_power) + 30,
        report_vs_raw_power_relative_error=error,
        report_vs_raw_power_match_at_1e_minus7=error <= 1e-7,
        run_started_utc=capture["run_started_utc"],
        run_returned_utc=returned,
        analysis_done_observed_utc=capture.get("analysis_done_observed_utc"),
        timestamps=timestamps,
        parameter_readbacks=readbacks,
        source_parameter_power_w=data(SOURCE + "Pwr"),
        raw_carrier=carrier,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest = load(args.manifest)
    profiles = {p["name"]: p for p in manifest["profiles"]}
    rows = []
    for source in manifest["captures"]:
        path = args.root / source["path"]
        if digest(path) != source["sha256"]:
            raise ValueError("Raw capture hash changed: " + str(path))
        item = normalize(load(path), profiles[source["profile"]])
        item["capture_source"] = source
        rows.append(item)
    plan = dict(profiles=manifest["profiles"], diagnostic_phase_absolute_tolerance_degrees=1e-5)
    observations = dict(samples=rows)
    result = dict(
        schema_version=1,
        classification="bounded_vendor_observations_not_compatibility",
        provenance_checks_passed=True,
        eligible_for_vendor_compatibility=False,
        samples=rows,
        linear_candidates=hypotheses.analyze(plan, observations),
        spline_report_axis=hypotheses.assess_natural_cubic(plan, observations),
        spline_raw_axis=hypotheses.assess_natural_cubic(plan, observations, "raw_output_w"),
    )
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")


if __name__ == "__main__":
    main()
