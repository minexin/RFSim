"""Audit controlled equal-power CW products; keep compression hypotheses diagnostic."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
from datetime import datetime

spec = importlib.util.spec_from_file_location(
    "single", Path(__file__).with_name("compare-single-compression.py"))
single = importlib.util.module_from_spec(spec)
spec.loader.exec_module(single)
BASE = single.BASE
SPECTRUM = "System1_Data/Eqns/VarBlock/"
# Frequency bin, order, exact source expression. IDs are deliberately not fixed.
PRODUCTS = (
    (1, 2, "-(Source.Source1)+(Source.Source2)"),
    (21, 2, "(Source.Source1)+(Source.Source2)"),
    (20, 2, "2x(Source.Source1)"),
    (22, 2, "2x(Source.Source2)"),
    (9, 3, "-(Source.Source2)+2x(Source.Source1)"),
    (12, 3, "-(Source.Source1)+2x(Source.Source2)"),
    (31, 3, "2x(Source.Source1)+(Source.Source2)"),
    (32, 3, "(Source.Source1)+2x(Source.Source2)"),
    (30, 3, "3x(Source.Source1)"),
    (33, 3, "3x(Source.Source2)"),
)


def node(capture, suffix):
    matches = [n for n in capture["nodes"] if n["path"] == BASE + suffix]
    if len(matches) != 1 or matches[0].get("evaluation_error"):
        raise ValueError("Missing/ambiguous/failed node: " + suffix)
    return matches[0]


def vector(capture, name):
    entry = node(capture, SPECTRUM + name)
    data = entry["data"]
    if not isinstance(data, list) or entry["dimensions"] != [len(data)]:
        raise ValueError("Invalid spectrum vector: " + name)
    return data


def inspect(capture, power_dbm):
    if not math.isfinite(power_dbm) or not -200 <= power_dbm <= 1 - 10 * math.log10(2):
        raise ValueError("Total two-tone input must not exceed sample input P1dB")
    single.runner.validate_capture(capture, "compression")
    times = [datetime.fromisoformat(re.sub(r"(\.\d{6})\d+Z$", r"\1Z", capture[key])[:-1]
                                   + "+00:00").timestamp()
             for key in ("run_started_utc", "run_returned_utc")]
    spectrum_time = int(node(capture, "System1_Data")["timestamp"])
    if not math.floor(times[0]) <= spectrum_time <= math.ceil(times[1]):
        raise ValueError("Stale main spectrum dataset")
    parts = [n["path"].split("/")[-1] for n in capture["nodes"]
             if n["path"].startswith(BASE + "Sch1/PartList/") and n["path"].count("/") == 4
             and n["path"].split("/")[-1] not in ("Page", "PartLib")]
    if sorted(parts) != ["Out", "RFAmp", "Source"]:
        raise ValueError("Expected isolated RFAMP topology")
    power = 10 ** ((power_dbm - 30) / 10)
    expected = {**single.PARAMETERS, "Source/Pwr": [power, power],
                "Source/Freq": [1e9, 1.1e9], "Source/Enable": [1, 1],
                "Source/SrcType": [0, 0], "Source/EnablePN": [0, 0],
                "Source/MultiCarrier": [0, 0], "Source/Phase": [0, 0],
                "Source/BW": [1e6, 1e6], "Source/Name": ["Source1", "Source2"]}

    def equal(actual, wanted):
        if isinstance(wanted, str):
            return actual == wanted
        return (isinstance(actual, (int, float)) and not isinstance(actual, bool)
                and math.isfinite(actual) and math.isclose(actual, wanted, rel_tol=1e-12))

    verified = []
    for name, wanted in expected.items():
        device, parameter = name.split("/")
        entry = node(capture, f"Sch1/PartList/{device}/ParamSet/{parameter}")
        actual = entry["data"]
        if isinstance(wanted, list):
            valid = (isinstance(actual, list) and len(actual) == len(wanted)
                     and all(equal(a, b) for a, b in zip(actual, wanted))
                     and entry["dimensions"] == ([2] if parameter == "Name" else [2, 1]))
        else:
            valid = equal(actual, wanted)
        if not valid:
            raise ValueError("Parameter mismatch: " + name)
        verified.append({"parameter": name, "value": actual})
    if node(capture, "System1/Path0/PathFreq")["data"] != 1e9:
        raise ValueError("Expected explicit 1 GHz measurement channel")
    cf = node(capture, "System1_Data_Folder/System1_Data_Path1/Eqns/VarBlock/CF")
    if cf["data"] != [1e9, 1e9] or cf["dimensions"] != [2]:
        raise ValueError("Unexpected channel measurement")
    return power, verified


def limiter_ratio(power):
    # The single-tone -4/-1 dB offsets are tested here as a hypothesis only.
    knee, limit, amplitude = math.sqrt(10 ** (-3.4)), math.sqrt(10 ** (-2.8)), math.sqrt(power)
    limited = amplitude if amplitude <= knee else knee + (limit - knee) * math.tanh(
        (amplitude - knee) / (limit - knee))
    return (limited / amplitude) ** 2


def compare(library, capture, power_dbm):
    power, parameters = inspect(capture, power_dbm)
    identifiers, names = vector(capture, "IDNo"), vector(capture, "IDName")
    if len(identifiers) != len(names) or len(set(identifiers)) != len(identifiers):
        raise ValueError("Invalid spectrum identity map")
    frequencies, powers, ids = (vector(capture, key) for key in ("F2", "P2", "ID2"))
    if not len(frequencies) == len(powers) == len(ids) or any(i not in identifiers for i in ids):
        raise ValueError("Mismatched spectrum arrays or unknown identity")

    def observed(expression, bin_index, order, direct=False):
        pattern = r"\{\d+\}" + ("D" if direct else "") + re.escape("[" + expression + "],"
                    + ("Source," if direct else "") + "RFAmp")
        matches = [(i, n) for i, n in zip(identifiers, names)
                   if isinstance(n, str) and re.fullmatch(pattern, n)]
        if len(matches) != 1:
            raise ValueError("Missing/ambiguous direct product: " + expression)
        identifier, name = matches[0]
        points = [(f, p) for f, p, i in zip(frequencies, powers, ids) if i == identifier]
        bounds = [bin_index * 1e8 - order / 2, bin_index * 1e8 + order / 2]
        if len(points) != 2 or [f for f, _ in points] != bounds:
            raise ValueError("Expected exact CW product boundaries")
        if any(not isinstance(p, (int, float)) or not math.isfinite(p) or p <= 0 for _, p in points):
            raise ValueError("Invalid product power")
        if not math.isclose(points[0][1], points[1][1], rel_tol=1e-12):
            raise ValueError("Non-flat product spectrum")
        return points[0][1], name, bounds

    native = library.intercept_amplifier(1e8, {10: math.sqrt(power), 11: math.sqrt(power)},
        power_gain_db=20, input_ip2_dbm=20, input_ip3_dbm=10)
    checks = []
    for bin_index, order, expression in PRODUCTS:
        measured, name, bounds = observed(expression, bin_index, order)
        predicted = abs(native[bin_index]) ** 2
        error = predicted / measured - 1
        checks.append({"bin": bin_index, "order": order, "identity": name,
                       "frequency_bounds_hz": bounds, "systemvue_power_w": measured,
                       "rfmodel_power_w": predicted, "signed_relative_error": error,
                       "passed": abs(error) <= 1e-7,
                       "inferred_effective_drive_ratio": (measured / predicted) ** (1 / order)})
    fundamental = abs(library.p1db_fundamental(math.sqrt(power), power_gain_db=20,
        output_p1db_dbm=20, total_incident_power_w=2 * power)) ** 2
    direct_checks = []
    for bin_index, source in ((10, "Source1"), (11, "Source2")):
        measured, name, _ = observed("(Source." + source + ")", bin_index, 1, direct=True)
        error = fundamental / measured - 1
        direct_checks.append({"bin": bin_index, "identity": name, "systemvue_power_w": measured,
                              "rfmodel_power_w": fundamental, "signed_relative_error": error,
                              "passed": abs(error) <= 1e-7})
    hypotheses = {}
    for mode, drive in (("per_tone", power), ("total_rf", 2 * power)):
        ratio = limiter_ratio(drive)
        hypotheses[mode] = {"input_power_ratio": ratio, "affects_compatibility_verdict": False,
            "checks": [{"bin": c["bin"], "signed_relative_error":
                       c["rfmodel_power_w"] * ratio ** c["order"] / c["systemvue_power_w"] - 1}
                       for c in checks]}
    return {"scope": "Equal-power 1.0/1.1 GHz CW direct fundamentals and ten unique H2/H3/IM2/IM3 products",
            "limitation": "Powers only; excludes overlapping carrier products, phase, noise and full RFAMP equivalence",
            "source_power_dbm_per_tone": power_dbm, "total_source_power_w": 2 * power,
            "parameters": parameters, "run_started_utc": capture["run_started_utc"],
            "run_returned_utc": capture["run_returned_utc"],
            "raw_capture_sha256": capture.get("raw_capture_sha256"),
            "manager_messages": capture["manager_errors"], "relative_tolerance": 1e-7,
            "checks": checks, "direct_fundamental_checks": direct_checks,
            "limiter_hypotheses": hypotheses,
            "passed": all(c["passed"] for c in checks + direct_checks)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    captures = json.loads(raw.decode("utf-8-sig"))
    if not isinstance(captures, list) or not captures:
        parser.error("Nonempty controlled sweep required")
    if len({c["source_power_dbm_per_tone"] for c in captures}) != len(captures):
        parser.error("Duplicate per-tone powers in controlled sweep")
    library = single.Library(args.library.resolve())
    reports = [compare(library, c, c["source_power_dbm_per_tone"]) for c in captures]
    report = {"reports": reports, "passed": all(r["passed"] for r in reports),
              "captures_sha256": hashlib.sha256(raw).hexdigest(),
              "library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest()}
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
