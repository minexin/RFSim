"""Compare native saturation response with diagnostic-only SystemVue captures."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "single", Path(__file__).with_name("compare-single-compression.py"))
single = importlib.util.module_from_spec(spec)
spec.loader.exec_module(single)


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty diagnostic sweep required")
    reports, seen = [], set()
    for capture in captures:
        power = capture["source_power_dbm"]
        saturation = capture["output_saturation_dbm"]
        identity = (power, saturation)
        if identity in seen:
            raise ValueError("Duplicate input/saturation point")
        seen.add(identity)
        verified = single.inspect_capture(capture, power, output_saturation_dbm=saturation,
                                          compression_diagnostic=True)
        output = abs(library.saturating_fundamental(math.sqrt(10 ** ((power - 30) / 10)),
            power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=saturation)) ** 2
        observed = verified["measurements"]["DCP"][1]
        residual = output / observed - 1
        reports.append({"source_power_dbm": power, "output_saturation_dbm": saturation,
                        "raw_capture_sha256": capture["raw_capture_sha256"],
                        "run_started_utc": verified["run_started_utc"],
                        "run_returned_utc": verified["run_returned_utc"],
                        "parameters": verified["parameters"],
                        "manager_messages": verified["manager_errors"],
                        "rfmodel_output_w": output, "systemvue_output_w": observed,
                        "signed_relative_error": residual,
                        "within_diagnostic_tolerance": abs(residual) <= 1e-7})
    return {"scope": "Native cubic / incremental-tanh fundamental diagnostics",
            "eligible_for_compatibility": False, "relative_tolerance": 1e-7,
            "limitation": "Warning-bearing measurements cannot establish strict RFAMP compatibility; harmonics are not modeled",
            "coefficients_fitted_to_capture": False, "reports": reports,
            "all_within_diagnostic_tolerance": all(r["within_diagnostic_tolerance"] for r in reports)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["all_within_diagnostic_tolerance"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
