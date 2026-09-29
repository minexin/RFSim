"""Audit RFAMP nonlinear settings in raw antenna captures; do not infer model equivalence."""
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

# Evaluated values use watts; DataEntry retains the vendor's dBm expressions.
EXPECTED = {"OIP2": 1e5, "OIP3": 1e4, "OP1dB": 1e3,
            "OPSAT": 10 ** 3.3, "AMtoPM": -5., "AMtoPM_Mode": 0,
            "PwrAMtoPM": .01, "EnablePN": 0}


def audit(capture, power_dbm):
    capture = dict(capture, source_power_dbm=power_dbm, parameters_verified=True)
    antenna.validate(capture)
    records = []
    for device in ("RFAmp1", "RFAmp2"):
        for parameter, expected in EXPECTED.items():
            path = antenna.parameter_path(device + "/" + parameter)
            matches = [node for node in capture["nodes"] if node["path"] == path]
            if len(matches) != 1:
                raise ValueError("Missing or ambiguous nonlinear parameter: " + path)
            node = matches[0]
            value = node.get("data")
            if (node.get("evaluation_error") or isinstance(value, bool)
                    or not isinstance(value, (int, float)) or not math.isfinite(value)
                    or not math.isclose(value, expected, rel_tol=1e-12, abs_tol=0)):
                raise ValueError("Unexpected nonlinear parameter: " + path)
            records.append({"device": device, "parameter": parameter, "value": value,
                            "data_entry": node.get("data_entry"),
                            "data_source": node.get("data_source")})
    return {"source_power_dbm": power_dbm, "run_started_utc": capture["run_started_utc"],
            "parameters": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="append", nargs=2, required=True,
                        metavar=("POWER_DBM", "RAW_CAPTURE"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() in {Path(path).resolve() for _, path in args.capture}:
        parser.error("Output must not overwrite raw capture")
    records = []
    seen = set()
    for power_text, path in args.capture:
        power = float(power_text)
        if power in seen:
            raise ValueError("Duplicate source power")
        seen.add(power)
        raw = Path(path).read_bytes()
        record = audit(json.loads(raw.decode("utf-8-sig")), power)
        record["capture_sha256"] = hashlib.sha256(raw).hexdigest()
        records.append(record)
    result = {"scope": "Verified RFAMP nonlinear settings across captured carrier powers",
              "limitation": "Parameter values do not establish the internal RFAMP transfer law",
              "points": sorted(records, key=lambda point: point["source_power_dbm"])}
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
