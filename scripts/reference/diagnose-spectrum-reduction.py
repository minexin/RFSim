"""Isolate SystemVue spectrum reduction from missing high-order polynomial origins."""

import argparse
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location(
    "reduction_highorder", Path(__file__).with_name("diagnose-highorder-amplifier.py")
)
highorder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(highorder)
comparison = highorder.comparison
POWERS = {(-10.0, -10.0), (-10.0, -20.0), (-20.0, -10.0)}
TIE_TOLERANCE = 1e-12


def signature(capture):
    return (
        capture["source_power_dbm"],
        capture["second_source_power_dbm"],
        capture["spectrum_reduction"],
    )


def observations(capture):
    result = {}
    for label, rows in comparison.spectrum(capture, 3).items():
        if not label.startswith("[") or not label.endswith("],RFAmp1"):
            continue
        counts = re.findall(r"(?:(\d+)x)?\(Source\.Source[12]\)", label)
        if sum(int(count) if count else 1 for count in counts) in (4, 5):
            result[label] = rows
    return result


def diagnose(library, references, captures):
    calibration = highorder.diagnose(library, references)
    if not (
        calibration["fourth_order_rule"]["consistent"]
        and calibration["fifth_order_hypothesis"]["consistent"]
    ):
        raise ValueError("Independent high-order reference hypothesis failed")
    coefficients = [
        0.0,
        0.0,
        0.0,
        0.0,
        calibration["fourth_order_rule"]["voltage_coefficient"],
        calibration["fifth_order_hypothesis"]["fitted_voltage_coefficient"],
    ]
    if not isinstance(captures, list):
        raise ValueError("Capture list required")
    indexed = {}
    for capture in captures:
        comparison.inspect(capture)
        if (
            type(capture.get("spectrum_reduction")) is not bool
            or "second_source_power_dbm" not in capture
            or not capture["two_tone"]
            or capture["source_phase_deg"] != 90
            or capture["maximum_order"] != 5
            or capture["second_gain_db"] != 10
            or capture["reverse_isolation_db"] != 140
            or capture["channel_bandwidth_hz"] != 1e6
            or capture["secondary_spectrum"]
            or capture["secondary_range_db"] != -50
        ):
            raise ValueError("Uncontrolled spectrum reduction experiment")
        key = signature(capture)
        if key in indexed:
            raise ValueError("Duplicate spectrum reduction experiment")
        indexed[key] = capture
    if set(indexed) != {(*powers, enabled) for powers in POWERS for enabled in (False, True)}:
        raise ValueError("Require all three power configurations with both reduction settings")

    cases, samples, predicted_by_case = [], {}, {}
    for key in sorted(indexed):
        capture = indexed[key]
        response = library.coherent_polynomial(
            1e8, highorder.source_components(capture), coefficients, reference_ohms=50.0
        )
        predicted, groups = {}, defaultdict(list)
        for term in response.terms:
            expression = comparison.traced.expression(
                tuple(
                    (1 if i > 0 else -1) * response.inputs[abs(i) - 1].bin
                    for i in term.input_indices
                )
            )
            label = comparison.label(expression, 1)
            if label in predicted:
                raise ValueError("Duplicate predicted high-order label")
            predicted[label] = term
            groups[(term.component.bin, term.component.bandwidth_hz)].append(label)
        observed = observations(capture)
        if set(observed) - set(predicted):
            raise ValueError("Unexpected high-order source identity")
        expected_labels = set(predicted)
        if capture["spectrum_reduction"]:
            expected_labels = set()
            for labels in groups.values():
                maximum = max(abs(predicted[label].component.amplitude) for label in labels)
                expected_labels.update(
                    label
                    for label in labels
                    if abs(predicted[label].component.amplitude) >= maximum * (1 - TIE_TOLERANCE)
                )
        checks, group_sum_checks = [], []
        for label, rows in observed.items():
            term = predicted[label]
            component = term.component
            rows = highorder.pair(rows, component.bin * 1e8, component.bandwidth_hz)
            error = max(
                abs(row[1] - component.amplitude) / abs(component.amplitude) for row in rows
            )
            checks.append(
                {
                    "label": label,
                    "order": term.order,
                    "bin": component.bin,
                    "expected_wave": [component.amplitude.real, component.amplitude.imag],
                    "observed_waves": [[row[1].real, row[1].imag] for row in rows],
                    "relative_wave_error": error,
                    "passed": error <= highorder.TOLERANCE,
                }
            )
            group = groups[(component.bin, component.bandwidth_hz)]
            if capture["spectrum_reduction"] and len(group) > 1:
                summed = sum(predicted[name].component.amplitude for name in group)
                error_sum = max(abs(row[1] - summed) / abs(summed) for row in rows)
                group_sum_checks.append(
                    {
                        "label": label,
                        "relative_wave_error": error_sum,
                        "passed": error_sum <= highorder.TOLERANCE,
                    }
                )
        cases.append(
            {
                "configuration": dict(
                    zip(("source_power_dbm", "second_source_power_dbm", "spectrum_reduction"), key)
                ),
                "raw_capture_sha256": capture["raw_capture_sha256"],
                "predicted_count": len(predicted),
                "observed_count": len(observed),
                "labels_match_control_hypothesis": set(observed) == expected_labels,
                "not_recorded": sorted(set(predicted) - set(observed)),
                "fourth_order": highorder.summarize([c for c in checks if c["order"] == 4]),
                "fifth_order": highorder.summarize([c for c in checks if c["order"] == 5]),
                "checks": checks,
                "full_group_sum_hypothesis": {
                    **highorder.summarize(group_sum_checks),
                    "matching_count": sum(c["passed"] for c in group_sum_checks),
                },
            }
        )
        samples[key] = observed
        predicted_by_case[key] = predicted

    pairs = []
    for powers in sorted(POWERS):
        enabled, disabled = samples[(*powers, True)], samples[(*powers, False)]
        common = set(enabled) & set(disabled)
        errors = [
            max(
                abs(a[1] - b[1]) / abs(b[1])
                for a, b in zip(sorted(enabled[label]), sorted(disabled[label]))
            )
            for label in sorted(common)
        ]
        restored = set(disabled) - set(enabled)
        pairs.append(
            {
                "source_powers_dbm": list(powers),
                "restored_count": len(restored),
                "restored_labels": sorted(restored),
                "enabled_labels_missing_when_disabled": sorted(set(enabled) - set(disabled)),
                "common_count": len(common),
                "max_relative_wave_change": max(errors, default=None),
                "retained_waves_unchanged": bool(common)
                and all(e <= highorder.TOLERANCE for e in errors),
                "full_expansion_recorded_when_disabled": set(disabled)
                == set(predicted_by_case[(*powers, False)]),
            }
        )
    consistent = all(
        c["labels_match_control_hypothesis"]
        and c["fourth_order"]["consistent"]
        and c["fifth_order"]["consistent"]
        for c in cases
    ) and all(
        p["retained_waves_unchanged"] and p["full_expansion_recorded_when_disabled"] for p in pairs
    )
    return {
        "classification": "diagnostic_only",
        "eligible_for_compatibility": False,
        "scope": "First-stage fourth/fifth-order RF origins in six controlled two-tone captures",
        "control_hypothesis_consistent": consistent,
        "relative_wave_tolerance": highorder.TOLERANCE,
        "dominance_tie_tolerance": TIE_TOLERANCE,
        "voltage_coefficients": coefficients,
        "calibration_note": "a4 follows the documented intercept guideline; a5 is fitted using a separate prior single-tone capture. No reduction experiment is used for fitting.",
        "cases": cases,
        "pairs": pairs,
        "limitations": [
            "Observed largest-amplitude label retention is a bounded diagnostic hypothesis, not a general reduction algorithm.",
            "The same-frequency/same-bandwidth group sum does not describe retained individual waves in these captures.",
            "No NodeTotal, path budget, noise, secondary mixing or second-stage acceptance is implied.",
            "No production polynomial terms are dropped or implicitly merged.",
            "Fifth-order coefficients remain parameter-specific and experimentally identified.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("references", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    inputs = [args.library, args.references, args.captures]
    if args.output.resolve() in {p.resolve() for p in inputs}:
        parser.error("Output must not overwrite inputs")
    reference_raw, capture_raw = args.references.read_bytes(), args.captures.read_bytes()
    report = diagnose(
        comparison.traced.two_tone.single.Library(args.library.resolve()),
        json.loads(reference_raw.decode("utf-8-sig")),
        json.loads(capture_raw.decode("utf-8-sig")),
    )
    report["references_sha256"] = hashlib.sha256(reference_raw).hexdigest()
    report["captures_sha256"] = hashlib.sha256(capture_raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["control_hypothesis_consistent"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
