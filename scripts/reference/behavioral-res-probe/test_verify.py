"""Adversarial checks against a real passing native probe result."""
import argparse
import copy
import json
from pathlib import Path
import tempfile

from verify import read_json, verify


def run(plan_path, result_path):
    baseline = verify(plan_path, result_path)
    actual = read_json(result_path)
    checks = []
    with tempfile.TemporaryDirectory(prefix="res-probe-negative-") as temp:
        mutated = Path(temp) / "mutated.json"

        def reject(name, change=None, raw=None):
            value = copy.deepcopy(actual)
            if change:
                change(value)
            mutated.write_text(raw if raw is not None else json.dumps(value), encoding="utf-8")
            try:
                verify(plan_path, mutated)
            except (ValueError, KeyError, TypeError) as error:
                checks.append({"name": name, "status": "rejected", "reason": str(error)})
            else:
                raise AssertionError(name + " was incorrectly accepted")

        reject("input_reference_plane_confusion", lambda x: x["path_power"].__setitem__("dut_input_accepted_w", .001))
        reject("RMS_peak_confusion", lambda x: x["loaded_channels"][3]["voltage_rms_v"].__setitem__("real", .15811388300841897))
        reject("output_current_sign", lambda x: x["loaded_channels"][3]["current_into_device_rms_a"].__setitem__("real", .0022360679774997894))
        reject("reflection_sign", lambda x: x["linear"]["S_row_major"][0].__setitem__("real", -.5))
        reject("missing_S12", lambda x: x["linear"]["S_row_major"].pop(1))
        reject("wrong_node_identity", lambda x: x["loaded_channels"][0].__setitem__("node", "OUT"))
        reject("source_reference_wrong", lambda x: x["fixture"].__setitem__("source_ohms", 100))
        reject("noise_claim_numeric_false", lambda x: x["fixture"].__setitem__("noise_validation_performed", 0))
        reject("power_as_boolean", lambda x: x["path_power"].__setitem__("load_delivered_w", False))
        # Inject invalid tokens without depending on one compiler's decimal
        # formatting or last-bit rounding of a physical result.
        body = Path(result_path).read_text(encoding="utf-8").lstrip()[1:]
        reject("nonfinite_token", raw='{"invalid_number":NaN,' + body)
        reject("binary64_overflow", raw='{"invalid_number":1e999,' + body)
        reject("duplicate_key", raw='{"schema_version":1,' + body)
    return {"status": "pass", "baseline_checks": baseline["checks_passed"],
            "negative_checks_passed": len(checks), "checks": checks}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--result", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(run(args.plan, args.result), indent=2, allow_nan=False))
