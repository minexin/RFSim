"""Summarize validated carrier sweeps without fitting or correcting the RFModel core."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "antenna", Path(__file__).with_name("compare-antenna.py"))
antenna = importlib.util.module_from_spec(spec)
spec.loader.exec_module(antenna)


def summarize(captures):
    if len(captures) < 2:
        raise ValueError("At least two independently captured power points are required")
    points = []
    powers, capture_hashes = set(), set()
    for capture in captures:
        if capture.get("parameters_verified") is not True:
            raise ValueError("Verified parameters are required for a power sweep")
        values = antenna.validate(capture)
        power = capture.get("source_power_dbm", -50.)
        digest = capture.get("capture_sha256")
        if not isinstance(digest, str) or len(digest) != 64 or any(
                character not in "0123456789abcdef" for character in digest):
            raise ValueError("Missing or invalid capture digest")
        if power in powers or digest in capture_hashes:
            raise ValueError("Duplicate power or capture")
        powers.add(power)
        capture_hashes.add(digest)
        points.append({"source_power_dbm": power, "source_power_w": 10 ** ((power - 30) / 10),
                       "capture_sha256": digest, "run_started_utc": capture["run_started_utc"],
                       "measurements": values})
    points.sort(key=lambda point: point["source_power_dbm"])
    baseline = points[0]
    low = baseline["measurements"]
    for point in points:
        point["nodes"] = []
        for index, name in enumerate(antenna.NAMES):
            values = point["measurements"]
            ratio = values["CGAIN"][index] / low["CGAIN"][index]
            point["nodes"].append({
                "name": name, "gain": values["CGAIN"][index],
                "gain_change_db_from_lowest_power": 10 * math.log10(ratio),
                "gain_change_fraction_from_lowest_power": ratio - 1,
                "noise_density_w_per_hz": values["CND"][index],
                "noise_density_change_fraction_from_lowest_power":
                    values["CND"][index] / low["CND"][index] - 1,
                "signal_output_w": values["DCP"][index],
                "signal_transducer_gain": values["DCP"][index] / point["source_power_w"],
                "noise_factor": values["CNF"][index],
            })
        del point["measurements"]
    return {"scope": "SystemVue antenna carrier-power sensitivity; no fitted model correction",
            "baseline_source_power_dbm": baseline["source_power_dbm"],
            "parameter_contract": "Only carrier Pwr[0] varies; other captured parameters validated",
            "limitation": "Observed power dependence does not uniquely identify RFAMP compression",
            "points": points}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.resolve() in {path.resolve() for path in args.captures}:
        parser.error("Output must not overwrite a capture")
    documents, inputs = [], []
    for path in args.captures:
        raw = path.read_bytes()
        documents.append(json.loads(raw.decode("utf-8-sig")))
        inputs.append({"file": path.name, "sha256": hashlib.sha256(raw).hexdigest()})
    result = summarize(documents)
    result["inputs"] = inputs
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
