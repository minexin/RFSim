"""Compare the native JSON compression chain with archived SystemVue signal data."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from rfmodel import Library
from rfmodel.spectrum_file import analyze_spectrum

NAMES = ("Source", "Attn1", "RFAmp1", "Attn2", "RFAmp2")
RELATIVE_TOLERANCE = 1e-7


def model(power_w, parameters):
    stages = []
    for index, (temperature, gain_db) in enumerate(((77.7, 25.), (453.6, 30.)), 1):
        amplitude = 1 / math.sqrt(1 + temperature / 290.)
        stages.append({
            "id": f"Attn{index}", "type": "linear_network",
            "network": {
                "devices": [{"id": "pad", "s": [[0, amplitude], [amplitude, 0]]}],
                "external_ports": [["pad", 0], ["pad", 1]],
            },
        })
        name = f"RFAmp{index}"
        output_p1db_w = parameters[name, "OP1dB"]
        if not math.isfinite(output_p1db_w) or output_p1db_w <= 0:
            raise ValueError("Invalid captured OP1dB")
        if parameters[name, "AMtoPM_Mode"] != 0:
            raise ValueError("This model requires disabled AM/PM")
        stages.append({
            "id": name, "type": "p1db_fundamental", "power_gain_db": gain_db,
            "output_p1db_dbm": 10 * math.log10(output_p1db_w) + 30,
        })
    return {
        "format": "rfmodel.spectrum-chain", "version": 1,
        "spacing_hz": 5e9, "reference_ohms": 50,
        "input": [{"bin": 1, "amplitude": [math.sqrt(power_w), 0]}],
        "stages": stages,
    }


def local_signal_diagnostic(library, document, nodes):
    """Condition each stage on measured preceding power, assuming matched waves."""
    records = []
    for index, stage in enumerate(document["stages"], 1):
        measured_input = nodes[index - 1]["signal_output_w"]
        measured_output = nodes[index]["signal_output_w"]
        local_document = dict(document)
        local_document["input"] = [{"bin": 1, "amplitude": [math.sqrt(measured_input), 0]}]
        local_document["stages"] = [stage]
        result = analyze_spectrum(library, local_document)
        predicted = result["stages"][0]["spectrum"][0]["power_w"]
        records.append({
            "node": stage["id"],
            "measured_input_w": measured_input,
            "measured_output_w": measured_output,
            "predicted_output_from_measured_input_w": predicted,
            "signed_relative_output_residual": predicted / measured_output - 1,
        })
    return records


def compare(library, sweep, settings):
    settings_by_hash = {point["capture_sha256"]: point for point in settings["points"]}
    hashes = [point["capture_sha256"] for point in sweep["points"]]
    if (not hashes or len(set(hashes)) != len(hashes) or
            len(settings_by_hash) != len(settings["points"]) or
            set(hashes) != set(settings_by_hash)):
        raise ValueError("Expected unique matching capture sets")
    checks = []
    local_points = []
    for point in sweep["points"]:
        setting = settings_by_hash[point["capture_sha256"]]
        if setting["source_power_dbm"] != point["source_power_dbm"]:
            raise ValueError("Mismatched capture power")
        power = point["source_power_w"]
        expected_power = 10 ** ((point["source_power_dbm"] - 30) / 10)
        if not math.isfinite(power) or power <= 0 or not math.isclose(power, expected_power, rel_tol=1e-12):
            raise ValueError("Inconsistent source power")
        if [node["name"] for node in point["nodes"]] != list(NAMES):
            raise ValueError("Unexpected reference node order")
        parameters = {(entry["device"], entry["parameter"]): entry["value"]
                      for entry in setting["parameters"]}
        if len(parameters) != len(setting["parameters"]):
            raise ValueError("Duplicate nonlinear parameter")
        document = model(power, parameters)
        result = analyze_spectrum(library, document)
        powers = [power]
        for stage in result["stages"]:
            spectrum = stage["spectrum"]
            if len(spectrum) != 1 or spectrum[0]["bin"] != 1:
                raise ValueError("Unexpected native output spectrum")
            powers.append(spectrum[0]["power_w"])
        for node, native_power in zip(point["nodes"], powers):
            for metric, native in (("gain", native_power / power), ("signal_output_w", native_power)):
                reference = node[metric]
                if not math.isfinite(reference) or reference <= 0:
                    raise ValueError("Invalid reference measurement")
                residual = native / reference - 1
                checks.append({
                    "source_power_dbm": point["source_power_dbm"],
                    "node": node["name"], "metric": metric,
                    "rfmodel": native, "systemvue": reference,
                    "signed_relative_error": residual,
                    "passed": math.isfinite(native) and abs(residual) <= RELATIVE_TOLERANCE,
                })
        # This diagnostic must never feed back into the end-to-end checks above.
        local_points.append({
            "source_power_dbm": point["source_power_dbm"],
            "capture_sha256": point["capture_sha256"],
            "stages": local_signal_diagnostic(library, document, point["nodes"]),
        })
    return {
        "scope": "Native JSON matched single-tone compression chain versus archived SystemVue signals",
        "excluded": ["noise", "saturation", "intermodulation", "AM/PM", "mismatch"],
        "limitation": "Results apply only to the captured powers and signal metrics, not full RFAMP compatibility",
        "relative_tolerance": RELATIVE_TOLERANCE,
        "passed": all(check["passed"] for check in checks), "checks": checks,
        "local_signal_diagnostic": {
            "scope": "Each native stage receives the measured preceding-node signal power",
            "assumption": "Preceding-node delivered power represents matched incident wave power",
            "limitation": "Not an isolated-device experiment; does not resolve reverse waves or mismatch",
            "affects_compatibility_verdict": False,
            "points": local_points,
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("sweep", type=Path)
    parser.add_argument("settings", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    inputs = (args.library, args.sweep, args.settings)
    if args.output.resolve() in [path.resolve() for path in inputs]:
        parser.error("Output must not overwrite input")
    raw = [path.read_bytes() for path in inputs]
    report = compare(Library(args.library.resolve()),
                     *(json.loads(value.decode("utf-8-sig")) for value in raw[1:]))
    report["input_sha256"] = dict(zip(("library", "sweep", "settings"),
                                     (hashlib.sha256(value).hexdigest() for value in raw)))
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
