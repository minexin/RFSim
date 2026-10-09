"""Check documented fourth-order RFAMP rules and a bounded fifth-order hypothesis."""

import argparse
import cmath
import hashlib
import importlib.util
import json
import math
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "highorder_cascade", Path(__file__).with_name("compare-cascade-amplifier.py")
)
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
from rfmodel import CoherentComponent, SpectrumKind, TwoToneIntercept, InterceptReference

CONFIGURATIONS = {
    (-30.0, 0.0, False),
    (-20.0, 0.0, False),
    (-10.0, 0.0, False),
    (-10.0, 45.0, False),
    (-10.0, 90.0, False),
    (-10.0, 90.0, True),
}
CALIBRATION = (-10.0, 0.0, False)
TOLERANCE = 1e-7


def signature(capture):
    return capture["source_power_dbm"], capture["source_phase_deg"], capture["two_tone"]


def source_components(capture):
    powers = [
        capture["source_power_dbm"],
        capture.get("second_source_power_dbm", capture["source_power_dbm"]),
    ]
    phases = (
        [0.0, capture["source_phase_deg"]] if capture["two_tone"] else [capture["source_phase_deg"]]
    )
    return [
        CoherentComponent(
            10 + i,
            SpectrumKind.SOURCE,
            1.0,
            i + 1,
            cmath.rect(math.sqrt(10.0 ** ((powers[i] - 30.0) / 10.0)), math.radians(phase)),
        )
        for i, phase in enumerate(phases)
    ]


def pair(rows, frequency, bandwidth):
    if len(rows) != 2:
        raise ValueError("Expected two band-edge samples per selected source spectrum")
    rows = sorted(rows)
    expected = (frequency - bandwidth / 2.0, frequency + bandwidth / 2.0)
    if any(abs(row[0] - target) > 1e-4 for row, target in zip(rows, expected)):
        raise ValueError("Unexpected high-order support frequency/bandwidth")
    if rows[0][2] != rows[1][2]:
        raise ValueError("Inconsistent high-order coherence group")
    return rows


def summarize(checks):
    return {
        "observed_count": len(checks),
        "consistent": all(c["passed"] for c in checks) if checks else None,
        "max_relative_wave_error": max((c["relative_wave_error"] for c in checks), default=None),
    }


def diagnose(library, captures):
    if not isinstance(captures, list):
        raise ValueError("Capture list required")
    indexed = {}
    for capture in captures:
        comparison.inspect(capture)
        if (
            capture["maximum_order"] != 5
            or capture["reverse_isolation_db"] != 140
            or capture["second_gain_db"] != 10
            or capture["channel_bandwidth_hz"] != 1e6
            or capture["secondary_spectrum"]
            or capture["secondary_range_db"] != -50
            or capture.get("spectrum_reduction", True) is not True
            or capture.get("second_source_power_dbm", capture["source_power_dbm"])
            != capture["source_power_dbm"]
        ):
            raise ValueError("Uncontrolled high-order diagnostic configuration")
        key = signature(capture)
        if key in indexed:
            raise ValueError("Duplicate high-order diagnostic configuration")
        indexed[key] = capture
    if set(indexed) != CONFIGURATIONS:
        raise ValueError("Require the complete power, phase and two-tone experiment set")

    calibration = indexed[CALIBRATION]
    label5 = comparison.label(comparison.traced.expression((10,) * 5), 1)
    observed = comparison.spectrum(calibration, 3)
    if label5 not in observed:
        raise ValueError("Missing fifth harmonic calibration observation")
    rows = pair(observed[label5], 5e9, 5.0)
    wave = source_components(calibration)[0].amplitude
    ratios = [row[1] / (5.0**4 * wave**5) for row in rows]
    if any(abs(value.imag) > TOLERANCE * abs(value) for value in ratios):
        raise ValueError("Calibration is inconsistent with a real voltage coefficient")
    coefficient5 = sum(value.real for value in ratios) / 2.0
    if not math.isfinite(coefficient5) or coefficient5 >= 0.0:
        raise ValueError("Unexpected fifth-order calibration sign/range")

    # Official Gain Compression and Intermod Generation: OIP4 = OP1dB + 13 dB.
    # Here fixed OP1dB=20 dBm and G=10 dB, verified by comparison.inspect.
    # The two-tone 3*f1-f2 term has four permutations.
    coefficient4 = library.polynomial_coefficients_from_intercepts(
        10.0,
        [TwoToneIntercept(3, -1, 33.0, 1, InterceptReference.OUTPUT)],
        reference_ohms=50.0,
    )[4]
    cases, all_fourth, fifth_holdouts = [], [], []
    for key in sorted(indexed):
        capture = indexed[key]
        sources = source_components(capture)
        response = library.coherent_polynomial(
            1e8, sources, [0.0, 0.0, 0.0, 0.0, coefficient4, coefficient5], reference_ohms=50.0
        )
        observed = comparison.spectrum(capture, 3)
        checks, missing, selected_labels = [], [], set()
        for term in response.terms:
            expression = comparison.traced.expression(
                tuple(
                    (1 if i > 0 else -1) * response.inputs[abs(i) - 1].bin
                    for i in term.input_indices
                )
            )
            label = comparison.label(expression, 1)
            if label in selected_labels:
                raise ValueError("Duplicate generated high-order label")
            selected_labels.add(label)
            if label not in observed:
                missing.append({"order": term.order, "label": label, "bin": term.component.bin})
                continue
            samples = pair(observed[label], term.component.bin * 1e8, term.component.bandwidth_hz)
            error = max(
                abs(row[1] - term.component.amplitude) / abs(term.component.amplitude)
                for row in samples
            )
            checks.append(
                {
                    "order": term.order,
                    "label": label,
                    "bin": term.component.bin,
                    "expected_wave": [term.component.amplitude.real, term.component.amplitude.imag],
                    "observed_waves": [[row[1].real, row[1].imag] for row in samples],
                    "relative_wave_error": error,
                    "passed": error <= TOLERANCE,
                }
            )
        fourth = [c for c in checks if c["order"] == 4]
        fifth = [c for c in checks if c["order"] == 5]
        if not fourth or (key[0] == -10 and not fifth):
            raise ValueError("Required high-order observations are missing")
        # Require complete single-tone coverage at the calibration power.
        # Multi-tone omissions are an observed gap, not an implicit zero or pass.
        if key[0] == -10 and not key[2] and missing:
            raise ValueError("Incomplete single-tone high-order coverage")
        for label in observed:
            if label.startswith("[") and label.endswith("],RFAmp1"):
                counts = re.findall(r"(?:(\d+)x)?\(Source\.Source[12]\)", label)
                order = sum(int(count) if count else 1 for count in counts)
                if order in (4, 5) and label not in selected_labels:
                    raise ValueError("Unexpected observed high-order source identity")
        all_fourth.extend(fourth)
        if key != CALIBRATION:
            fifth_holdouts.extend(fifth)
        cases.append(
            {
                "configuration": dict(
                    zip(("source_power_dbm", "source_phase_deg", "two_tone"), key)
                ),
                "fifth_order_role": "calibration" if key == CALIBRATION else "holdout",
                "fourth_order": summarize(fourth),
                "fifth_order": summarize(fifth),
                "all_predicted_origins_recorded": not missing,
                "not_recorded": missing,
                "checks": checks,
                "raw_capture_sha256": capture["raw_capture_sha256"],
            }
        )
    return {
        "classification": "diagnostic_only",
        "eligible_for_compatibility": False,
        "scope": "First-stage RFAMP generated fourth/fifth-order source spectra only",
        "relative_wave_tolerance": TOLERANCE,
        "all_predicted_origins_recorded": all(not case["not_recorded"] for case in cases),
        "two_tone_missing_origins": next(
            case["not_recorded"] for case in cases if case["configuration"]["two_tone"]
        ),
        "fourth_order_rule": {
            "output_ip4_dbm": 33.0,
            "input_ip4_dbm": 23.0,
            "voltage_coefficient": coefficient4,
            **summarize(all_fourth),
        },
        "fifth_order_hypothesis": {
            "calibration_configuration": list(CALIBRATION),
            "fitted_voltage_coefficient": coefficient5,
            **summarize(fifth_holdouts),
        },
        "cases": cases,
        "limitations": [
            "Fifth-order coefficient is fitted for one amplifier parameter configuration.",
            "Unrecorded terms are not zero and are excluded from numeric checks.",
            "Two-tone source-spectrum omissions remain unresolved even at the calibration power.",
            "No high-order compression, second-stage, noise, or complete sub-spectrum acceptance.",
            "No production RFAMP coefficient was changed by this diagnostic.",
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
    report = diagnose(
        comparison.traced.two_tone.single.Library(args.library.resolve()),
        json.loads(raw.decode("utf-8-sig")),
    )
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return (
        0
        if (
            report["fourth_order_rule"]["consistent"]
            and report["fifth_order_hypothesis"]["consistent"]
        )
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
