"""Diagnose SystemVue 2023 RFAMP unity gain without changing nominal acceptance."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "cascade_comparison", Path(__file__).with_name("compare-cascade-amplifier.py")
)
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)

SCAN_GAINS = (-1.0, -0.001, -1e-6, -1e-8, 0.0, 1e-8, 1e-6, 0.001, 1.0, 5.0, 10.0, 20.0)
HOLDOUTS = ((-30, 0, False), (-10, 45, False), (-30, 90, True), (-10, 90, True))
EQUIVALENT_GAIN_DB = 10 * math.log10(1.00000023)


def summary(report):
    checks = report["checks"]
    return {
        "passed": report["passed"],
        "check_count": len(checks),
        "failed_check_count": sum(not item["passed"] for item in checks),
        "max_relative_complex_error": max(
            item.get("relative_complex_error", 0.0) for item in checks
        ),
        "max_relative_power_error": max(
            abs(item.get("relative_power_error", 0.0)) for item in checks
        ),
        "max_relative_drive_error": max(item.get("relative_error", 0.0) for item in checks),
    }


class GainHypothesis:
    """Diagnostic parameter transform; the normal Library/API is never modified."""

    def __init__(self, library, effective_gain_db):
        self.library = library
        self.effective_gain_db = effective_gain_db

    def __getattr__(self, name):
        return getattr(self.library, name)

    def coherent_amplifier(self, *args, **kwargs):
        if kwargs["power_gain_db"] == 0:
            # Hold output intercepts fixed while changing the assumed forward gain.
            kwargs = dict(
                kwargs,
                power_gain_db=self.effective_gain_db,
                input_ip2_dbm=kwargs["input_ip2_dbm"] - self.effective_gain_db,
                input_ip3_dbm=kwargs["input_ip3_dbm"] - self.effective_gain_db,
            )
        return self.library.coherent_amplifier(*args, **kwargs)


def diagnose(library, captures):
    # This checks all ordinary parameter, topology, freshness and spectrum gates.
    nominal = comparison.compare(library, captures)
    indexed = {}
    for capture in captures:
        if (
            capture["reverse_isolation_db"] != 140
            or capture["maximum_order"] != 3
            or capture["channel_bandwidth_hz"] != 1e6
            or capture["secondary_spectrum"]
            or capture["secondary_range_db"] != -50
        ):
            raise ValueError("Uncontrolled gain diagnostic configuration")
        signature = (
            capture["source_power_dbm"],
            capture["source_phase_deg"],
            capture["two_tone"],
            capture["second_gain_db"],
        )
        indexed[signature] = capture

    # Require both sides of zero, the independent holdouts, and an explicit
    # equivalent-gain capture. Absence must not become a vacuous success.
    expected = {(-20, 90, False, gain) for gain in SCAN_GAINS}
    expected.update((*settings, 0.0) for settings in HOLDOUTS)
    equivalent = [
        key
        for key in indexed
        if key[:3] == (-20, 90, False)
        and math.isclose(key[3], EQUIVALENT_GAIN_DB, rel_tol=1e-8, abs_tol=0.0)
    ]
    if len(equivalent) != 1 or set(indexed) != expected | set(equivalent):
        raise ValueError("Incomplete or unexpected gain sweep/holdout coverage")

    zero = indexed[(-20, 90, False, 0.0)]
    near = indexed[(-20, 90, False, 1e-8)]
    zero_spectrum = comparison.spectrum(zero, 2)
    near_spectrum = comparison.spectrum(near, 2)
    estimates = []
    for expression, order in (
        ("2x(Source.Source1)", 2),
        ("3x(Source.Source1)", 3),
        ("-(Source.Source1)+2x(Source.Source1)", 3),
    ):
        identity = comparison.label(expression, 2)
        ratio = zero_spectrum[identity][0][1] / near_spectrum[identity][0][1]
        # Below the distortion limiter, fixed output intercepts give a generated
        # order-n wave proportional to linear power gain ** (n / 2).
        estimate = 10 ** (near["second_gain_db"] / 10) * abs(ratio) ** (2 / order)
        estimates.append(
            {
                "identity": identity,
                "order": order,
                "linear_power_gain": estimate,
                "ratio_phase_radians": math.atan2(ratio.imag, ratio.real),
            }
        )
    # Fit only H2 from the -20 dBm single-tone pair. H3 and all other cases
    # are independent checks, not additional fitting observations.
    inferred_gain = estimates[0]["linear_power_gain"]
    effective_gain_db = 10 * math.log10(inferred_gain)
    agreement = all(
        math.isclose(item["linear_power_gain"], inferred_gain, rel_tol=1e-12)
        and abs(item["ratio_phase_radians"]) <= 1e-12
        for item in estimates
    )
    adjusted = comparison.compare(GainHypothesis(library, effective_gain_db), captures)

    equivalent_spectrum = comparison.spectrum(indexed[equivalent[0]], 2)
    if set(zero_spectrum) != set(equivalent_spectrum):
        raise ValueError("Equivalent gain changed spectrum identities")
    equality_checks = []
    for identity, rows in zero_spectrum.items():
        other = equivalent_spectrum[identity]
        if len(rows) != len(other):
            raise ValueError("Equivalent gain changed spectrum dimensions")
        for first, second in zip(rows, other):
            if first[0] != second[0] or first[2] != second[2]:
                raise ValueError("Equivalent gain changed frequency or coherence")
            equality_checks.append(abs(first[1] - second[1]) / abs(first[1]))
    explicit_gain_matches = max(equality_checks) <= 1e-12

    cases = []
    for original, transformed in zip(nominal["reports"], adjusted["reports"]):
        cases.append(
            {
                "configuration": original["configuration"],
                "raw_capture_sha256": original["raw_capture_sha256"],
                "nominal": summary(original),
                "gain_hypothesis": summary(transformed),
            }
        )
    return {
        "classification": "diagnostic_only",
        "eligible_for_compatibility": False,
        "nominal_passed": nominal["passed"],
        "nominal_relative_tolerance": nominal["relative_tolerance"],
        "fit_capture_sha256": [zero["raw_capture_sha256"], near["raw_capture_sha256"]],
        "gain_estimates": estimates,
        "inferred_linear_power_gain": inferred_gain,
        "inferred_gain_db": effective_gain_db,
        "estimates_agree": agreement,
        "explicit_equivalent_capture_sha256": indexed[equivalent[0]]["raw_capture_sha256"],
        "explicit_equivalent_max_relative_wave_difference": max(equality_checks),
        "explicit_equivalent_matches": explicit_gain_matches,
        "hypothesis_consistent": agreement and explicit_gain_matches and adjusted["passed"],
        "cases": cases,
        "limitations": [
            "Empirical behavior in SystemVue 2023.0.0.11903, not a verified internal implementation",
            "Only matched RFAMP cascades, 140 dB isolation, tested gain/power/phase settings",
            "No extrapolation to other versions, devices, mismatch, noise or compressed drive",
            "Core APIs retain the requested gain; this diagnostic does not grant nominal compatibility",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    library = comparison.traced.two_tone.single.Library(args.library.resolve())
    report = diagnose(library, json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["hypothesis_consistent"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
