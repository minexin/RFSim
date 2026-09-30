"""Validate and compare the controlled single-RFAMP reference capture."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from rfmodel import Library

spec = importlib.util.spec_from_file_location(
    "runner", Path(__file__).with_name("run-systemvue-reference.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
BASE = "RFModel_AmplifierCompression/Designs/"
PARAMETERS = {
    "RFAmp/G": 100., "RFAmp/NF": 10 ** .3, "RFAmp/RISO": 1e10,
    "RFAmp/ZIN": 50., "RFAmp/ZOUT": 50., "RFAmp/OIP2": 10.,
    "RFAmp/OIP3": 1., "RFAmp/OP1dB": .1, "RFAmp/OPSAT": 10 ** (-.7),
    "Source/R": 50., "Out/ZO": 50., "Source/Freq": 1e9,
    "Source/Enable": [1], "Source/SrcType": [0], "Source/EnablePN": [0],
}
PROFILES = {"sample": (20, 20, 1e9), "antenna": (30, 60, 5e9), "limiter": (10, 15, 1e9)}


def inspect_capture(capture, source_power_dbm, *, reverse_isolation_db=100, profile="sample",
                    output_saturation_dbm=None, compression_diagnostic=False):
    if profile not in PROFILES:
        raise ValueError("Unknown compression profile")
    if output_saturation_dbm is not None and (
            profile != "sample" or output_saturation_dbm not in (22, 23, 26)):
        raise ValueError("Expected controlled sample-profile saturation of 22, 23 or 26 dBm")
    gain_db, output_p1db_dbm, frequency = PROFILES[profile]
    if reverse_isolation_db not in (50, 100):
        raise ValueError("Expected controlled reverse isolation of 50 or 100 dB")
    if compression_diagnostic and profile != "sample":
        raise ValueError("Compression diagnostic requires sample profile")
    maximum_input = 30 if compression_diagnostic else output_p1db_dbm - gain_db + 1
    if not math.isfinite(source_power_dbm) or not -200 <= source_power_dbm <= maximum_input:
        raise ValueError("Input outside controlled reference range")
    runner.validate_capture(capture, "compression", allow_compression_warning=compression_diagnostic)

    def node(path):
        matches = [entry for entry in capture["nodes"] if entry["path"] == BASE + path]
        if len(matches) != 1 or matches[0].get("evaluation_error"):
            raise ValueError("Missing/ambiguous/failed node: " + path)
        return matches[0]

    parts = [entry["path"].split("/")[-1] for entry in capture["nodes"]
             if entry["path"].startswith(BASE + "Sch1/PartList/")
             and entry["path"].count("/") == 4
             and entry["path"].split("/")[-1] not in ("Page", "PartLib")]
    if sorted(parts) != ["Out", "RFAmp", "Source"]:
        raise ValueError("Expected single-amplifier topology")
    power = 10 ** ((source_power_dbm - 30) / 10)
    verified = []
    expected_parameters = {**PARAMETERS, "Source/Pwr": power,
                           "RFAmp/RISO": 10 ** (reverse_isolation_db / 10)}
    if profile == "antenna":
        expected_parameters.update({"RFAmp/G": 1000., "RFAmp/NF": 1 + 700 / 290,
                                    "RFAmp/OIP2": 1e5, "RFAmp/OIP3": 1e4,
                                    "RFAmp/OP1dB": 1000., "RFAmp/OPSAT": 10 ** 3.3,
                                    "Source/Freq": frequency})
    if output_saturation_dbm is not None:
        expected_parameters["RFAmp/OPSAT"] = 10 ** ((output_saturation_dbm - 30) / 10)
    if profile == "limiter":
        expected_parameters.update({"RFAmp/G": 10., "RFAmp/OP1dB": 10 ** (-1.5),
                                    "RFAmp/OPSAT": 10 ** (-1.2)})
    for key, expected in expected_parameters.items():
        device, parameter = key.split("/")
        entry = node(f"Sch1/PartList/{device}/ParamSet/{parameter}")
        actual = entry["data"]
        if isinstance(expected, list):
            valid = actual == expected
        else:
            valid = (isinstance(actual, (int, float)) and not isinstance(actual, bool)
                     and math.isfinite(actual) and math.isclose(actual, expected, rel_tol=1e-12))
        if not valid:
            raise ValueError("Parameter mismatch: " + key)
        verified.append({"parameter": key, "value": actual, "data_entry": entry.get("data_entry")})
    measurements = {}
    for name in ("CF", "CGAIN", "DCP"):
        entry = node("System1_Data_Folder/System1_Data_Path1/Eqns/VarBlock/" + name)
        data = entry["data"]
        if (entry["dimensions"] != [2] or not isinstance(data, list) or len(data) != 2
                or any(not isinstance(v, (int, float)) or not math.isfinite(v) or v <= 0 for v in data)):
            raise ValueError("Invalid two-node measurement: " + name)
        measurements[name] = data
    if measurements["CF"] != [frequency, frequency]:
        raise ValueError("Unexpected carrier frequency")
    return {
        "scope": "Independent single RFAMP with explicit 50 ohm boundaries and controlled reverse isolation",
        "reverse_isolation_db": reverse_isolation_db, "profile": profile,
        "limitation": "Signal powers only; default higher-order/internal settings are not fully audited",
        "source_power_dbm": source_power_dbm, "parameters": verified,
        "run_started_utc": capture["run_started_utc"], "run_returned_utc": capture["run_returned_utc"],
        "manager_errors": capture["manager_errors"], "measurements": measurements,
        "eligible_for_compatibility": not compression_diagnostic,
    }


def compare(library, capture, source_power_dbm, *, reverse_isolation_db=100, profile="sample",
            output_saturation_dbm=None):
    verified = inspect_capture(capture, source_power_dbm, reverse_isolation_db=reverse_isolation_db,
                               profile=profile, output_saturation_dbm=output_saturation_dbm)
    gain_db, output_p1db_dbm, _ = PROFILES[profile]
    power = 10 ** ((source_power_dbm - 30) / 10)
    measurements = verified["measurements"]
    output = abs(library.p1db_fundamental(math.sqrt(power), power_gain_db=gain_db,
                                         output_p1db_dbm=output_p1db_dbm)) ** 2
    checks = []
    for name, predicted, observed in (("gain", output / power, measurements["CGAIN"][1]),
                                       ("output_w", output, measurements["DCP"][1])):
        residual = predicted / observed - 1
        checks.append({"metric": name, "rfmodel": predicted, "systemvue": observed,
                       "signed_relative_error": residual, "passed": abs(residual) <= 1e-7})
    return {
        **verified,
        "relative_tolerance": 1e-7, "passed": all(check["passed"] for check in checks), "checks": checks,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source-power-dbm", type=float, required=True)
    parser.add_argument("--reverse-isolation-db", type=int, choices=(50, 100), default=100)
    parser.add_argument("--profile", choices=PROFILES, default="sample")
    parser.add_argument("--output-saturation-dbm", type=int, choices=(22, 23, 26))
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.capture.resolve()):
        parser.error("Output must not overwrite an input")
    raw = args.capture.read_bytes()
    report = compare(Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")),
                     args.source_power_dbm, reverse_isolation_db=args.reverse_isolation_db,
                     profile=args.profile, output_saturation_dbm=args.output_saturation_dbm)
    report["capture_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
