"""Reduce recorded COM captures to compact, independently recheckable reference data."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "compare_attenuator", Path(__file__).with_name("compare-attenuator.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def collect(capture, loss):
    prefix = "RFModel_AttenuatorNoise/Designs/"
    def get(path):
        matches = [node for node in capture["nodes"] if node["path"] == path]
        if len(matches) != 1:
            raise ValueError("Missing or ambiguous measurement path: " + path)
        return matches[0]

    actual_loss_factor = get(prefix + "Sch1/PartList/Attn/ParamSet/L")["data"]
    if not math.isclose(actual_loss_factor, 10 ** (loss / 10), rel_tol=1e-12):
        raise ValueError("Requested attenuation did not take effect")
    temperature_c = get(prefix + "System1/RoomTemp")["data"]
    if not math.isclose(temperature_c + 273.15, 290, abs_tol=1e-10):
        raise ValueError("Unexpected analysis temperature")
    dataset = prefix + "System1_Data_Folder/System1_Sch1_Data_Path1"
    measurements = []
    for name in ("CF", "CGAIN", "CNF", "CND", "DCP"):
        node = get(dataset + "/Eqns/VarBlock/" + name)
        measurements.append({key: node[key] for key in ("path", "data", "dimensions")})
    result = {
        "run_started_utc": capture["run_started_utc"],
        "run_returned_utc": capture["run_returned_utc"],
        "manager_errors": capture["manager_errors"],
        "dataset_timestamp": get(dataset)["timestamp"],
        "parameters": {"frequency_hz": 100000000, "loss_db": loss, "temperature_k": 290,
                       "reference_ohms": 50, "source_available_w": 1e-19},
        "observed_loss_factor": actual_loss_factor,
        "observed_room_temperature_c": temperature_c,
        "measurements": measurements,
    }
    comparison.validate_reference(result, loss)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture_directory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--losses", nargs="+", default=["0", "1", "3", "10"],
                        help="Loss values; spelling must match attenuator-scan-<value>.json")
    args = parser.parse_args()
    samples = []
    for loss_text in args.losses:
        loss = float(loss_text)
        if not math.isfinite(loss) or not 0 <= loss <= 100:
            raise ValueError("Invalid attenuation")
        if any(sample["parameters"]["loss_db"] == loss for sample in samples):
            raise ValueError("Duplicate attenuation")
        if Path(loss_text).name != loss_text or "/" in loss_text or "\\" in loss_text:
            raise ValueError("Loss argument must not be a path")
        path = args.capture_directory / ("attenuator-scan-" + loss_text + ".json")
        content = path.read_bytes()
        sample = collect(json.loads(content.decode("utf-8-sig")), loss)
        sample["capture_sha256"] = hashlib.sha256(content).hexdigest()
        samples.append(sample)
    result = {"schema_version": 1, "product": "SystemVue 2023.0.0.11903", "samples": samples,
              "scope": "Live System1 attenuation scan; one temperature and frequency"}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Validated and collected " + str(len(samples)) + " fresh analysis captures")


if __name__ == "__main__":
    main()
