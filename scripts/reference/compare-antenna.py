"""Collect and replay the five-node, four-stage SystemVue antenna noise case."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess

spec = importlib.util.spec_from_file_location(
    "runner", Path(__file__).with_name("run-systemvue-reference.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
BASE = "RFModel_AntennaNoise/RF Design/"
DATASET = BASE + "System1_Data_Folder/System1_Data_Path1"
NAMES = ["Source", "Attn1", "RFAmp1", "Attn2", "RFAmp2"]
METRICS = {"CGAIN": "gain", "CNF": "noise_factor", "CND": "output_noise_w_per_hz",
           "DCP": "signal_output_w"}


def validate(capture):
    runner.validate_capture(capture, "antenna")

    def unique(path):
        nodes = [node for node in capture["nodes"] if node["path"] == path]
        if len(nodes) != 1:
            raise ValueError("Missing or ambiguous path: " + path)
        return nodes[0]

    temperature = unique(BASE + "System1/RoomTemp")["data"]
    if not isinstance(temperature, (int, float)) or not math.isclose(temperature, 16.85, abs_tol=1e-10):
        raise ValueError("Unexpected temperature")
    values = {}
    for name in ["CF", *METRICS]:
        node = unique(DATASET + "/Eqns/VarBlock/" + name)
        if node["dimensions"] != [5] or len(node["data"]) != 5:
            raise ValueError("Expected five nodes")
        if not all(isinstance(x, (int, float)) and math.isfinite(x) and x > 0 for x in node["data"]):
            raise ValueError("Invalid measurement")
        values[name] = node["data"]
    if values["CF"] != [5e9] * 5:
        raise ValueError("Unexpected carrier frequency")
    return values


def collect(capture):
    validate(capture)
    paths = {DATASET, BASE + "System1/RoomTemp"}
    paths.update(DATASET + "/Eqns/VarBlock/" + key for key in ["CF", *METRICS])
    result = {key: capture[key] for key in ("run_started_utc", "run_returned_utc", "manager_errors")}
    result["nodes"] = [node for node in capture["nodes"] if node["path"] in paths]
    for node in result["nodes"]:
        for key in ("methods", "variables"):
            node.pop(key, None)
    result["scope"] = "Official antenna example; matched small-signal RFModel approximation only"
    result["condition_limit"] = "Carrier and ambient temperature verified live; device settings mapped from vendor workspace, not fully read back"
    return result


def compare(reference, actual):
    values = validate(reference)
    if [node["name"] for node in actual["nodes"]] != NAMES:
        raise ValueError("Unexpected RFModel node order")
    checks = []
    for index, node in enumerate(actual["nodes"]):
        for measurement, key in METRICS.items():
            observed = node[key]
            if not isinstance(observed, (int, float)) or not math.isfinite(observed) or observed <= 0:
                raise ValueError("Invalid RFModel result")
            expected = values[measurement][index]
            error = abs(observed - expected) / expected
            checks.append({"node": NAMES[index], "measurement": measurement, "rfmodel": observed,
                           "systemvue": expected, "relative_error": error,
                           "relative_tolerance": 1e-7, "passed": error <= 1e-7})
    return {"checks": checks, "passed": all(check["passed"] for check in checks),
            "scope": reference["scope"], "condition_limit": reference["condition_limit"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["collect", "compare"])
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--executable", type=Path)
    args = parser.parse_args()
    raw = args.input.read_bytes()
    source = json.loads(raw.decode("utf-8-sig"))
    if args.mode == "collect":
        result = collect(source)
        result["capture_sha256"] = hashlib.sha256(raw).hexdigest()
    else:
        if args.executable is None:
            parser.error("compare requires --executable")
        process = subprocess.run([str(args.executable.resolve())], capture_output=True, text=True, check=True)
        result = compare(source, json.loads(process.stdout))
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.mode == "compare":
        print(str(sum(check["passed"] for check in result["checks"])) + "/20 checks passed")
        return 0 if result["passed"] else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
