"""Test a continuity-derived saturation hypothesis against warning-bearing diagnostics.

This does not implement the vendor model or confer compatibility acceptance.
"""
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


def continuous_tanh_power(input_w, gain, output_p1db_w, saturation_w):
    """Cubic below P1dB, tanh above with matching value and amplitude derivative."""
    if any(not math.isfinite(v) or v <= 0 for v in (gain, output_p1db_w, saturation_w)):
        raise ValueError("Positive finite gain and power anchors required")
    if saturation_w <= output_p1db_w or not math.isfinite(input_w) or input_w < 0:
        raise ValueError("Invalid input or saturation anchor")
    ratio = 10 ** (-1 / 20)
    amplitude_gain = math.sqrt(gain)
    x1 = math.sqrt(output_p1db_w / gain) / ratio
    x = math.sqrt(input_w)
    if x <= x1:
        return (amplitude_gain * x * (1 - (1 - ratio) * (x / x1) ** 2)) ** 2
    y1 = math.sqrt(output_p1db_w)
    limit = math.sqrt(saturation_w)
    slope = amplitude_gain * (3 * ratio - 2)
    scale = slope / (limit * (1 - output_p1db_w / saturation_w))
    shift = math.atanh(y1 / limit)
    return (limit * math.tanh(scale * (x - x1) + shift)) ** 2


def diagnose(captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty diagnostic sweep required")
    reports = []
    seen = set()
    for capture in captures:
        power_dbm = capture["source_power_dbm"]
        if power_dbm in seen:
            raise ValueError("Duplicate power point")
        seen.add(power_dbm)
        inspected = single.inspect_capture(capture, power_dbm, compression_diagnostic=True)
        if not inspected["manager_errors"]:
            raise ValueError("This diagnostic expects an explicit vendor compression warning")
        parameters = {entry["parameter"]: entry["value"] for entry in inspected["parameters"]}
        predicted = continuous_tanh_power(
            parameters["Source/Pwr"], parameters["RFAmp/G"],
            parameters["RFAmp/OP1dB"], parameters["RFAmp/OPSAT"])
        observed = inspected["measurements"]["DCP"][1]
        reports.append({"source_power_dbm": power_dbm,
                        "raw_capture_sha256": capture["raw_capture_sha256"],
                        "parameters": inspected["parameters"],
                        "run_started_utc": inspected["run_started_utc"],
                        "run_returned_utc": inspected["run_returned_utc"],
                        "manager_messages": inspected["manager_errors"],
                        "systemvue_output_w": observed, "candidate_output_w": predicted,
                        "signed_relative_error": predicted / observed - 1})
    return {
        "scope": "Warning-bearing single RFAMP saturation diagnosis only",
        "eligible_for_compatibility": False,
        "hypothesis": "Cubic below P1dB; shifted tanh above, matching amplitude value and derivative, asymptote sqrt(OPSAT)",
        "coefficients_fitted_to_capture": False,
        "limitation": "Vendor explicitly reports lower spectrum/measurement accuracy; no compatibility pass/fail is issued",
        "reports": reports,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() == args.captures.resolve():
        parser.error("Output must not overwrite input")
    raw = args.captures.read_bytes()
    report = diagnose(json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
