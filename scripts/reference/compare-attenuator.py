"""Compare captured SystemVue measurements with the actual RFModel C++ probe."""
import argparse
import json
import math
import re
import subprocess
from datetime import datetime
from pathlib import Path


def parse_utc(value):
    if not value.endswith("Z"):
        raise ValueError("Run timestamps must be UTC")
    # .NET round-trip timestamps have seven fractional digits; Python 3.9 accepts six.
    normalized = re.sub(r"(\.\d{6})\d+Z$", r"\1Z", value)
    return datetime.fromisoformat(normalized.replace("Z", "+00:00"))


def validate_reference(reference, loss_db=1):
    if not isinstance(loss_db, (int, float)) or not math.isfinite(loss_db) or not 0 <= loss_db <= 100:
        raise ValueError("Invalid attenuation")
    expected_parameters = {"frequency_hz": 100000000, "loss_db": loss_db, "temperature_k": 290,
                           "reference_ohms": 50, "source_available_w": 1e-19}
    if reference["parameters"] != expected_parameters or reference["manager_errors"]:
        raise ValueError("Reference conditions differ from the fixed C++ probe")
    started = parse_utc(reference["run_started_utc"])
    returned = parse_utc(reference["run_returned_utc"])
    timestamp = int(reference["dataset_timestamp"])
    if returned < started or not math.floor(started.timestamp()) <= timestamp <= math.ceil(returned.timestamp()):
        raise ValueError("Dataset timestamp does not belong to the recorded run")
    values = {}
    for measurement in reference["measurements"]:
        name = measurement["path"].rsplit("/", 1)[1]
        data = measurement["data"]
        if name in values or measurement["dimensions"] != [2] or len(data) != 2:
            raise ValueError("Unexpected reference vector: " + name)
        if not all(isinstance(value, (int, float)) and math.isfinite(value) for value in data):
            raise ValueError("Nonfinite reference vector: " + name)
        values[name] = data[-1]
        if name == "CF" and data != [100000000, 100000000]:
            raise ValueError("Reference carrier frequencies do not match")
    if set(values) != {"CF", "CGAIN", "CNF", "CND", "DCP"}:
        raise ValueError("Incomplete reference measurements")
    return values


def compare_sample(reference, executable, loss_db=1):
    values = validate_reference(reference, loss_db)
    completed = subprocess.run([str(executable.resolve()), str(loss_db)], check=True,
                               capture_output=True, text=True)
    actual = json.loads(completed.stdout)
    checks = []
    for name, key in [("gain", "CGAIN"), ("noise_factor", "CNF"),
                      ("output_noise_w_per_hz", "CND"), ("signal_output_w", "DCP")]:
        observed, baseline = actual[name], values[key]
        if not math.isfinite(observed) or not math.isfinite(baseline) or baseline <= 0:
            raise ValueError("Invalid numeric result: " + name)
        relative_error = abs(observed - baseline) / baseline
        checks.append({"measurement": name, "rfmodel": observed, "systemvue": baseline,
                       "relative_error": relative_error, "relative_tolerance": 1e-7,
                       "passed": relative_error <= 1e-7})
    return {"checks": checks, "passed": all(check["passed"] for check in checks)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding="utf-8-sig"))
    if "samples" in reference:
        if not reference["samples"]:
            raise ValueError("Empty reference scan")
        samples = []
        for sample in reference["samples"]:
            loss = sample["parameters"]["loss_db"]
            samples.append({"loss_db": loss, **compare_sample(sample, args.executable, loss)})
        result = {"reference": args.reference.as_posix(), "samples": samples,
                  "passed": all(sample["passed"] for sample in samples),
                  "scope": "Matched attenuator loss scan at 100 MHz and 290 K only"}
    else:
        result = {"reference": args.reference.as_posix(),
                  **compare_sample(reference, args.executable),
                  "scope": "One matched 1 dB attenuator at 100 MHz and 290 K; not library compatibility"}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
