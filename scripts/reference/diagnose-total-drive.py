"""Diagnose compression using measured total RF drive; not an end-to-end verdict."""
import argparse
import copy
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from rfmodel import Library

spec = importlib.util.spec_from_file_location("antenna", Path(__file__).with_name("compare-antenna.py"))
antenna = importlib.util.module_from_spec(spec)
spec.loader.exec_module(antenna)


def diagnose(library, capture, source_power_dbm):
    capture = copy.deepcopy(capture)
    capture["source_power_dbm"] = source_power_dbm
    capture["parameters_verified"] = True
    measurements = antenna.validate(capture)

    def value(path):
        matches = [node for node in capture["nodes"] if node["path"] == path]
        if len(matches) != 1 or matches[0].get("evaluation_error"):
            raise ValueError("Missing or invalid total-drive evidence: " + path)
        return matches[0]["data"]

    base = antenna.BASE + "System1_Data/Eqns/VarBlock/"
    names, powers = value(base + "RFElemList"), value(base + "RFPwrIn")
    if not isinstance(names, list) or not isinstance(powers, list) or len(names) != len(powers):
        raise ValueError("Mismatched RF element and power arrays")
    parts = [name.split("\\")[0] if isinstance(name, str) else None for name in names]
    if sorted(parts, key=str) != ["Attn1", "Attn2", "RFAmp1", "RFAmp2"]:
        raise ValueError("Unexpected RF elements")
    if any(isinstance(p, bool) or not isinstance(p, (int, float)) or not math.isfinite(p) or p <= 0 for p in powers):
        raise ValueError("Invalid total RF powers")
    output_p1db = value(antenna.parameter_path("RFAmp2/OP1dB"))
    if not isinstance(output_p1db, (int, float)) or not math.isclose(output_p1db, 1000., rel_tol=1e-12):
        raise ValueError("Unexpected output P1dB")
    total = powers[parts.index("RFAmp2")]
    fundamental = measurements["DCP"][3]
    measured_output = measurements["DCP"][4]
    if total < fundamental:
        raise ValueError("Total drive is below the measured fundamental")
    baseline_output = abs(library.p1db_fundamental(
        math.sqrt(fundamental), power_gain_db=30, output_p1db_dbm=60)) ** 2
    compressed_gain = abs(library.p1db_fundamental(
        math.sqrt(total), power_gain_db=30, output_p1db_dbm=60)) ** 2 / total
    predicted = compressed_gain * fundamental
    return {
        "source_power_dbm": source_power_dbm,
        "run_started_utc": capture["run_started_utc"],
        "element": names[parts.index("RFAmp2")],
        "fundamental_input_w": fundamental, "total_rf_input_w": total,
        "additional_drive_w": total - fundamental,
        "measured_output_w": measured_output,
        "fundamental_only_prediction_w": baseline_output,
        "total_drive_prediction_w": predicted,
        "fundamental_only_relative_residual": baseline_output / measured_output - 1,
        "total_drive_relative_residual": predicted / measured_output - 1,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--capture", nargs=2, action="append", required=True,
                        metavar=("POWER_DBM", "CAPTURE"))
    args = parser.parse_args()
    inputs = [args.library, *(Path(path) for _, path in args.capture)]
    if args.output.resolve() in {path.resolve() for path in inputs}:
        parser.error("Output must not overwrite inputs")
    library = Library(args.library.resolve())
    points = []
    seen = set()
    for power, path in args.capture:
        declared = float(power)
        if declared in seen:
            raise ValueError("Duplicate input power")
        seen.add(declared)
        raw = Path(path).read_bytes()
        result = diagnose(library, json.loads(raw.decode("utf-8-sig")), declared)
        result["capture_sha256"] = hashlib.sha256(raw).hexdigest()
        points.append(result)
    report = {
        "scope": "Conditional diagnostic using measured all-terminal total RF input power",
        "limitation": "Does not predict total drive or distinguish harmonics, reverse waves and other signals",
        "affects_compatibility_verdict": False,
        "library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest(),
        "points": sorted(points, key=lambda point: point["source_power_dbm"]),
    }
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
