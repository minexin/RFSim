"""Compare fundamental/H2/H3 with strict, warning-free single-tone references."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "harmonics", Path(__file__).with_name("compare-single-harmonics.py"))
harmonics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harmonics)


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty controlled sweep required")
    reports, seen = [], set()
    for capture in captures:
        power, profile = capture["source_power_dbm"], capture["profile"]
        saturation = capture["output_saturation_dbm"] if profile == "sample" else None
        identity = (profile, power, saturation)
        if identity in seen:
            raise ValueError("Duplicate controlled case")
        seen.add(identity)
        original = harmonics.compare(library, capture, power, profile=profile,
                                     output_saturation_dbm=saturation)
        verified = harmonics.single.inspect_capture(capture, power, profile=profile,
                                                    output_saturation_dbm=saturation)
        parameters = {p["parameter"]: p["value"] for p in verified["parameters"]}
        if not math.isclose(capture["output_saturation_dbm"],
                            30 + 10 * math.log10(parameters["RFAmp/OPSAT"]), abs_tol=1e-12):
            raise ValueError("Capture saturation label disagrees with verified parameters")
        gain = 10 * math.log10(parameters["RFAmp/G"])
        output = library.single_tone_amplifier(parameters["Source/Freq"],
            {1: math.sqrt(parameters["Source/Pwr"])}, power_gain_db=gain,
            output_p1db_dbm=30 + 10 * math.log10(parameters["RFAmp/OP1dB"]),
            output_saturation_dbm=30 + 10 * math.log10(parameters["RFAmp/OPSAT"]),
            input_ip2_dbm=30 + 10 * math.log10(parameters["RFAmp/OIP2"]) - gain,
            input_ip3_dbm=30 + 10 * math.log10(parameters["RFAmp/OIP3"]) - gain)
        checks = []
        observations = [(1, verified["measurements"]["DCP"][1])]
        observations += [(c["harmonic"], c["systemvue_power_w"]) for c in original["checks"]]
        for order, observed in observations:
            predicted = abs(output[order]) ** 2
            residual = predicted / observed - 1
            checks.append({"order": order, "rfmodel_power_w": predicted, "systemvue_power_w": observed,
                           "signed_relative_error": residual, "passed": abs(residual) <= 1e-7})
        reports.append({"profile": profile, "source_power_dbm": power,
                        "output_saturation_dbm": capture["output_saturation_dbm"],
                        "raw_capture_sha256": capture["raw_capture_sha256"],
                        "run_started_utc": verified["run_started_utc"],
                        "manager_messages": verified["manager_errors"],
                        "parameters": verified["parameters"],
                        "independent_parameter_cross_validation": profile == "limiter",
                        "unlimited_harmonic_checks": original["checks"], "checks": checks})
    return {"scope": "Controlled single-tone fundamental/H2/H3 powers only, with strict message rejection",
            "limitation": "Not full RFAMP equivalence; no phase, multitone, reverse-wave or noise validation",
            "limiter_offset_origin": "-4/-1 dB offsets identified from sample captures, then tested on a separate parameter profile",
            "relative_tolerance": 1e-7, "reports": reports,
            "passed": all(c["passed"] for r in reports for c in r["checks"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(harmonics.single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
