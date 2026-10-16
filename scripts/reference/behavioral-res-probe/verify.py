"""Independent developer oracle for one frozen native RES-chain fixture.

No RFModel imports, solver calls, vendor aliases, or vendor acceptance counts.
The plan hash binds the oracle, reference planes, and frozen configuration.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


PLAN_SHA256 = "4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e"
PLAN_ID = "systemvue-linear-rfsystem-minimal-20261016-v1"
ATOL = 1e-15
RTOL = 1e-12


def fail(message):
    raise ValueError(message)


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            fail("duplicate JSON key: " + key)
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"),
                      parse_constant=lambda value: fail("nonfinite token: " + value),
                      parse_float=lambda value: scalar(float(value)),
                      object_pairs_hook=no_duplicates)


def scalar(value):
    if type(value) not in (float, int) or not math.isfinite(value):
        fail("expected finite numeric scalar")
    return float(value)


def phasor(value):
    if type(value) is not dict or set(value) != {"real", "imag"}:
        fail("expected real/imag complex object")
    return complex(scalar(value["real"]), scalar(value["imag"]))


def identical(actual, expected):
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, dict):
        return (actual.keys() == expected.keys() and
                all(identical(actual[key], value) for key, value in expected.items()))
    if isinstance(expected, list):
        return (len(actual) == len(expected) and
                all(identical(a, b) for a, b in zip(actual, expected)))
    return actual == expected


def verify(plan_path, result_path):
    if hashlib.sha256(Path(plan_path).read_bytes()).hexdigest() != PLAN_SHA256:
        fail("frozen plan byte hash changed; new plan requires explicit review")
    plan = read_json(plan_path)
    result = read_json(result_path)
    checks = []

    def same(actual, expected, name):
        if not identical(actual, expected):
            fail(name + ": identity/type mismatch")
        checks.append({"name": name, "kind": "identity", "status": "pass"})

    def near(actual, expected, name, is_complex=False):
        actual = phasor(actual) if is_complex else scalar(actual)
        expected = complex(expected) if is_complex else scalar(expected)
        error = abs(actual - expected)
        limit = ATOL + RTOL * abs(expected)
        if error > limit:
            fail(f"{name}: difference {error:.17g} exceeds {limit:.17g}")
        checks.append({"name": name, "kind": "complex" if is_complex else "scalar",
                       "status": "pass", "absolute_error": error, "limit": limit})

    same(plan["plan_id"], PLAN_ID, "frozen_plan_id")
    same(result["schema_version"], 1, "schema_version")
    same(result["plan_id"], PLAN_ID, "native_plan_id")
    same(result["provenance_class"], "RFModel_installed_SDK_native_result_not_vendor_data", "provenance")
    f = result["fixture"]
    same(f, {"frequency_hz": 100000000, "source_available_w": .001,
             "source_phase_deg": 0, "source_ohms": 50, "load_ohms": 50,
             "resistors": [{"name": "R1", "ohms": 50}, {"name": "R2", "ohms": 50}],
             "ordered_nodes": ["IN", "MID", "OUT"],
             "connection": {"first": "R1/p1", "second": "R2/p0", "node": "MID"},
             "noise_policy": "omit_intrinsic_noise", "noise_validation_performed": False,
             "deterministic_single_cw": True, "vendor_channel_integration_performed": False},
         "frozen_fixture")
    same(result["conventions"], {
        "complex_encoding": "binary64_roundtrip_real_imag",
        "voltage": "RMS_phasor_to_common_ground_V",
        "current": "RMS_phasor_entering_named_device_port_A",
        "wave": "power_wave_sqrt_W_incident_into_device_outgoing_from_device",
        "net_power": "positive_into_named_device_port_W",
        "phase_reference": "real_positive_source_incident_wave",
        "peak_voltage": "sqrt_2_times_abs_loaded_RMS_phasor_single_CW_only",
        "vendor_measurement_aliases": False}, "observable_conventions")

    # The elementary circuit equations are independent of both native solvers.
    rf = plan["rf_system_analysis"]
    rs = rf["source"]["impedance_ohms"]["real"]
    rl = rf["load"]["impedance_ohms"]["real"]
    r1, r2 = [d["resistance_ohms"] for d in plan["fixture"]["devices"]]
    available = rf["source"]["available_power_w"]
    vs = 2 * math.sqrt(rs * available)
    current = vs / (rs + r1 + r2 + rl)
    voltages = [current * (r1 + r2 + rl), current * (r2 + rl), current * (r2 + rl), current * rl]
    oracle = plan["oracle"]
    near(current, oracle["current_rms_a"], "independent_current_matches_frozen_oracle")
    linear = result["linear"]
    near(linear["frequency_hz"], plan["fixture"]["frequency_hz"], "linear_frequency")
    same(linear["port_order"], ["IN", "OUT"], "linear_port_order")
    same(linear["S_order"], "row_response_column_excitation", "linear_S_order")
    same(linear["S_shape"], [2, 2], "linear_S_shape")
    same(linear["noise_requested"], False, "linear_noise_omission")
    same(len(linear["reference_ohms"]), 2, "linear_reference_count")
    same(len(linear["S_row_major"]), 4, "linear_S_entry_count")
    for i, reference in enumerate(linear["reference_ohms"]):
        near(reference, 50, f"linear_reference_{i}", True)
    for i, entry in enumerate(linear["S_row_major"]):
        near(entry, complex(oracle["DUT_S"]["row_major_real"][i],
                            oracle["DUT_S"]["row_major_imag"][i]), f"linear_S_{i}", True)
    same(len(result["loaded_channels"]), 4, "loaded_channel_count")
    for i, channel in enumerate(result["loaded_channels"]):
        node = ["IN", "MID", "MID", "OUT"][i]
        prefix = f"R{i // 2 + 1}/p{i % 2}"
        for key, expected in {"device": f"R{i // 2 + 1}", "device_index": i // 2,
                              "port": f"p{i % 2}", "port_index": i % 2, "node": node,
                              "bin": 1, "internally_connected": i in (1, 2)}.items():
            same(channel[key], expected, prefix + "_" + key)
        near(channel["frequency_hz"], plan["fixture"]["frequency_hz"], prefix + "_frequency")
        near(channel["reference_ohms"], 50, prefix + "_reference", True)
        v = voltages[i]
        ci = current if i % 2 == 0 else -current
        a = (v + rs * ci) / (2 * math.sqrt(rs))
        b = (v - rs * ci) / (2 * math.sqrt(rs))
        for key, expected in {"voltage_rms_v": v, "current_into_device_rms_a": ci,
                              "incident_wave_sqrt_w": a, "outgoing_wave_sqrt_w": b}.items():
            near(channel[key], expected, prefix + "_" + key, True)
        for key, expected in {"incident_wave_power_w": a * a, "outgoing_wave_power_w": b * b,
                              "net_into_device_w": v * ci,
                              "loaded_single_cw_peak_voltage_v": math.sqrt(2) * abs(v)}.items():
            near(channel[key], expected, prefix + "_" + key)
    expected_powers = {
        "source_available_w": available,
        "dut_input_accepted_w": current * current * (r1 + r2 + rl),
        "mid_delivered_to_R2_and_load_w": current * current * (r2 + rl),
        "load_delivered_w": current * current * rl,
        "load_delivered_dbm": 10 * math.log10(current * current * rl / .001),
        "load_over_source_available_ratio": current * current * rl / available,
        "load_over_dut_input_accepted_ratio": rl / (r1 + r2 + rl),
        "R1_dissipated_w": current * current * r1,
        "R2_dissipated_w": current * current * r2,
    }
    same(set(result["path_power"]), set(expected_powers), "path_power_fields")
    for key, expected in expected_powers.items():
        near(result["path_power"][key], expected, key)
    return {"schema_version": 1, "status": "pass", "plan_id": PLAN_ID,
            "plan_sha256": PLAN_SHA256,
            "native_result_sha256": hashlib.sha256(Path(result_path).read_bytes()).hexdigest(),
            "oracle_kind": "independent_series_circuit_and_frozen_full_S",
            "native_only_tolerance": {"atol": ATOL, "rtol": RTOL},
            "vendor_comparisons_performed": 0, "checks_passed": len(checks), "checks": checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--result", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.plan, args.result), indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
