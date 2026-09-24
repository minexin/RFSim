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


def collect(capture, loss, temperature_k=290, source_power_dbm=None):
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
    if not math.isclose(temperature_c + 273.15, temperature_k, rel_tol=0, abs_tol=1e-10):
        raise ValueError("Unexpected analysis temperature")
    source_power = 1e-19 if source_power_dbm is None else 10 ** ((source_power_dbm - 30) / 10)
    source_path = prefix + "Sch1/PartList/Source/ParamSet/Pwr"
    observed_power = None
    if source_power_dbm is not None or any(node["path"] == source_path for node in capture["nodes"]):
        observed_power = get(source_path)["data"]
        if not isinstance(observed_power, (int, float)) or not math.isclose(
                observed_power, source_power, rel_tol=1e-12, abs_tol=0):
            raise ValueError("Requested source power did not take effect")
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
        "parameters": {"frequency_hz": 100000000, "loss_db": loss, "temperature_k": temperature_k,
                       "reference_ohms": 50, "source_available_w": source_power},
        "observed_loss_factor": actual_loss_factor,
        "observed_room_temperature_c": temperature_c,
        "measurements": measurements,
    }
    if observed_power is not None:
        result["observed_source_available_w"] = observed_power
    comparison.validate_reference(result, loss, temperature_k, source_power)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture_directory", type=Path)
    parser.add_argument("output", type=Path)
    axis = parser.add_mutually_exclusive_group()
    axis.add_argument("--losses", nargs="+",
                      help="Loss values; spelling must match attenuator-scan-<value>.json")
    axis.add_argument("--temperatures", nargs="+",
                      help="Kelvin values at 1 dB; filenames attenuator-temperature-<value>.json")
    axis.add_argument("--powers-dbm", nargs="+",
                      help="Source dBm at 1 dB and 290 K; filenames attenuator-power-<value>.json")
    args = parser.parse_args()
    samples = []
    values = args.powers_dbm or args.temperatures or args.losses or ["0", "1", "3", "10"]
    seen = set()
    for value_text in values:
        value = float(value_text)
        if args.powers_dbm:
            valid = math.isfinite(value) and -200 <= value <= 30
        elif args.temperatures:
            valid = math.isfinite(value) and 0 < value <= 1000
        else:
            valid = math.isfinite(value) and 0 <= value <= 100
        if not valid:
            raise ValueError("Invalid scan value")
        if value in seen:
            raise ValueError("Duplicate scan value")
        seen.add(value)
        if Path(value_text).name != value_text or "/" in value_text or "\\" in value_text:
            raise ValueError("Scan argument must not be a path")
        prefix = ("attenuator-power-" if args.powers_dbm else
                  "attenuator-temperature-" if args.temperatures else "attenuator-scan-")
        path = args.capture_directory / (prefix + value_text + ".json")
        content = path.read_bytes()
        capture = json.loads(content.decode("utf-8-sig"))
        if args.powers_dbm:
            sample = collect(capture, 1, 290, value)
        else:
            sample = collect(capture, 1, value) if args.temperatures else collect(capture, value)
        sample["capture_sha256"] = hashlib.sha256(content).hexdigest()
        samples.append(sample)
    result = {"schema_version": 1, "product": "SystemVue 2023.0.0.11903", "samples": samples,
              "scope": ("Live System1 source power scan at 1 dB, 290 K, 100 MHz" if args.powers_dbm else
                        "Live System1 temperature scan at 1 dB and 100 MHz; diagnostic only"
                        if args.temperatures else
                        "Live System1 attenuation scan; one temperature and frequency")}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Validated and collected " + str(len(samples)) + " fresh analysis captures")


if __name__ == "__main__":
    main()
