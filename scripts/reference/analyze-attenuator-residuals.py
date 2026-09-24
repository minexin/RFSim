"""Separate source-level offsets from through-path ratios in captured measurements.

This is diagnostic evidence, not a replacement for absolute RFModel comparisons.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "compare_attenuator", Path(__file__).with_name("compare-attenuator.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def analyze(sample):
    loss = sample["parameters"]["loss_db"]
    comparison.validate_reference(sample, loss, sample["parameters"]["temperature_k"],
                                  sample["parameters"]["source_available_w"])
    values = {item["path"].rsplit("/", 1)[-1]: item["data"] for item in sample["measurements"]}
    signal_in, signal_out = values["DCP"]
    noise_in, noise_out = values["CND"]
    gain_reported = values["CGAIN"][1]
    if min(signal_in, signal_out, noise_in, noise_out, gain_reported) <= 0:
        raise ValueError("Residual analysis requires positive powers and gain")
    expected_gain = 10 ** (-loss / 10)
    measured_gain = signal_out / signal_in
    source_power = sample["parameters"]["source_available_w"]
    temperature = sample["parameters"]["temperature_k"]
    metrics = {
        "source_signal_offset": signal_in / source_power - 1,
        "source_noise_offset_from_si_kT": noise_in / (1.380649e-23 * temperature) - 1,
        "signal_transfer_residual": measured_gain / expected_gain - 1,
        "noise_output_to_input_residual": noise_out / noise_in - 1,
        "reported_gain_vs_signal_ratio_residual": gain_reported / measured_gain - 1,
    }
    if not all(math.isfinite(value) for value in metrics.values()):
        raise ValueError("Residual overflow")
    return {
        "loss_db": loss,
        "temperature_k": temperature,
        "source_available_w": source_power,
        "source_signal_w": signal_in,
        "source_noise_w_per_hz": noise_in,
        "inferred_source_noise_over_temperature": noise_in / temperature,
        "inference_limit": "Noise divided by temperature is not proof of the simulator's internal constant",
        "signal_gain_from_two_DCP_nodes": measured_gain,
        "reported_CGAIN": gain_reported,
        "signed_relative_residuals": metrics,
        "signal_transfer_within_1e_7": abs(metrics["signal_transfer_residual"]) <= 1e-7,
        "noise_output_to_input_within_1e_7": abs(metrics["noise_output_to_input_residual"]) <= 1e-7,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding="utf-8-sig"))
    if not reference["samples"]:
        raise ValueError("Empty scan")
    result = {
        "reference": args.reference.as_posix(),
        "scope": "Diagnostic decomposition only; absolute comparison failures remain unresolved",
        "samples": [analyze(sample) for sample in reference["samples"]],
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for sample in result["samples"]:
        print(sample["loss_db"], sample["temperature_k"], {key: value * 1e6 for key, value in
                                  sample["signed_relative_residuals"].items()})


if __name__ == "__main__":
    main()
