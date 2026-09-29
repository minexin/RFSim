"""Test a P1dB-calibrated cubic hypothesis; never modify the compatibility verdict."""
import argparse
import hashlib
import json
import math
from pathlib import Path


def cubic_power(input_w, gain, output_p1db_w):
    if any(not math.isfinite(value) or value <= 0 for value in (input_w, gain, output_p1db_w)):
        raise ValueError("Positive finite powers and gain required")
    amplitude_at_p1db = 10 ** (-1 / 20)
    input_p1db = output_p1db_w / (gain * amplitude_at_p1db ** 2)
    if input_w > input_p1db:
        raise ValueError("Diagnostic is restricted to below P1dB")
    amplitude_ratio = 1 - (1 - amplitude_at_p1db) * input_w / input_p1db
    return gain * input_w * amplitude_ratio ** 2


def diagnose(sweep, settings):
    settings_by_hash = {point["capture_sha256"]: point for point in settings["points"]}
    if len(settings_by_hash) != len(settings["points"]):
        raise ValueError("Duplicate settings capture")
    results = []
    for point in sweep["points"]:
        settings_point = settings_by_hash.get(point["capture_sha256"])
        if settings_point is None or settings_point["source_power_dbm"] != point["source_power_dbm"]:
            raise ValueError("Missing matching nonlinear parameter capture")
        parameters = {(entry["device"], entry["parameter"]): entry["value"]
                      for entry in settings_point["parameters"]}
        power = point["source_power_w"]
        power /= 1 + 77.7 / 290
        power = cubic_power(power, 10 ** 2.5, parameters["RFAmp1", "OP1dB"])
        power /= 1 + 453.6 / 290
        power = cubic_power(power, 1000., parameters["RFAmp2", "OP1dB"])
        actual = point["nodes"][-1]["gain"]
        predicted = power / point["source_power_w"]
        results.append({"source_power_dbm": point["source_power_dbm"],
                        "predicted_gain": predicted, "systemvue_gain": actual,
                        "signed_relative_gain_residual": predicted / actual - 1})
    return {"hypothesis": "Matched cascaded fundamental cubic calibrated only from G and OP1dB",
            "not_implemented": ["saturation", "intermodulation", "AM/PM", "noise", "mismatch"],
            "limitation": "Agreement cannot establish proprietary RFAMP algorithm equivalence",
            "points": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sweep", type=Path)
    parser.add_argument("settings", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.sweep.resolve(), args.settings.resolve()):
        parser.error("Output must not overwrite input")
    raw = [path.read_bytes() for path in (args.sweep, args.settings)]
    result = diagnose(*(json.loads(value.decode("utf-8-sig")) for value in raw))
    result["input_sha256"] = [hashlib.sha256(value).hexdigest() for value in raw]
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
