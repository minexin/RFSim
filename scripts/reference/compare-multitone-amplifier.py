"""Compare native separated multitone components with strictly validated RFAMP captures."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location("two_tone", Path(__file__).with_name("compare-two-tone.py"))
two_tone = importlib.util.module_from_spec(spec)
spec.loader.exec_module(two_tone)


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty controlled sweep required")
    reports, seen = [], set()
    for capture in captures:
        power_dbm = capture["source_power_dbm_per_tone"]
        if power_dbm in seen:
            raise ValueError("Duplicate per-tone power")
        seen.add(power_dbm)
        original = two_tone.compare(library, capture, power_dbm)
        power = 10 ** ((power_dbm - 30) / 10)
        native = library.multitone_amplifier(1e8, {10: math.sqrt(power), 11: math.sqrt(power)},
            power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=23,
            input_ip2_dbm=20, input_ip3_dbm=10)
        checks = []
        for original_check in original["checks"] + original["direct_fundamental_checks"]:
            order = original_check.get("order", 1)
            family = (native.direct, native.second_order, native.third_order)[order - 1]
            index = original_check["bin"]
            observed = original_check["systemvue_power_w"]
            predicted = abs(family[index]) ** 2
            error = predicted / observed - 1
            checks.append({"order": order, "bin": index, "identity": original_check["identity"],
                           "systemvue_power_w": observed, "rfmodel_power_w": predicted,
                           "signed_relative_error": error, "passed": abs(error) <= 1e-7})
        reports.append({"source_power_dbm_per_tone": power_dbm,
                        "raw_capture_sha256": capture["raw_capture_sha256"],
                        "run_started_utc": capture["run_started_utc"],
                        "manager_messages": capture["manager_errors"],
                        "parameters": original["parameters"],
                        "total_input_power_w": native.total_input_power_w,
                        "limited_input_power_w": native.limited_input_power_w,
                        "unlimited_passed": original["passed"],
                        "unlimited_product_checks": original["checks"], "checks": checks})
    return {"scope": "Native direct carriers and ten non-overlapping second/third-order powers per equal-tone case",
            "limitation": "Does not validate carrier-overlapping products, phase, unequal tones, high orders, feedback or noise",
            "relative_tolerance": 1e-7, "reports": reports,
            "passed": all(check["passed"] for r in reports for check in r["checks"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(two_tone.single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
