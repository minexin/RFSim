"""Audit RFAMP orders 4..11 using documented even IPs and isolated odd calibration."""

import argparse
import hashlib
import importlib.util
import json
import math
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "eleventh_reference_helpers", Path(__file__).with_name("diagnose-highorder-amplifier.py")
)
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

comparison = helpers.comparison
TOLERANCE = 1e-7
EVEN_OFFSETS_DB = {4: 13, 6: 11, 8: 9, 10: 8}
ODD_ORDERS = (5, 7, 9, 11)
# maximum order, first power, phase, two-tone flag, second power
CALIBRATION = (11, 5, 0, False, 5)
CONFIGURATIONS = {
    CALIBRATION,
    (11, 0, 0, False, 0),
    (11, 5, 45, False, 5),
    (11, 5, 90, True, 5),
    (11, 5, 90, True, 0),
    (7, 5, 0, False, 5),
    (9, 5, 0, False, 5),
}


def signature(capture):
    return tuple(
        capture[k]
        for k in (
            "maximum_order",
            "source_power_dbm",
            "source_phase_deg",
            "two_tone",
            "second_source_power_dbm",
        )
    )


def term_label(term, inputs):
    indices = tuple(
        (1 if index > 0 else -1) * inputs[abs(index) - 1].bin for index in term.input_indices
    )
    return comparison.label(comparison.traced.expression(indices), 1)


def diagnose(library, captures):
    if not isinstance(captures, list):
        raise ValueError("Capture list required")
    indexed = {}
    for capture in captures:
        comparison.inspect(capture)
        if (
            capture["second_gain_db"] != -10
            or capture["reverse_isolation_db"] != 140
            or capture["channel_bandwidth_hz"] != 1e6
            or capture["secondary_spectrum"]
            or capture["secondary_range_db"] != -50
            or capture.get("spectrum_reduction") is not False
        ):
            raise ValueError("Uncontrolled eleventh-order experiment")
        key = signature(capture)
        if key in indexed:
            raise ValueError("Duplicate eleventh-order configuration")
        indexed[key] = capture
    if set(indexed) != CONFIGURATIONS:
        raise ValueError("Require all seven power, phase, tone and maximum-order configurations")

    calibration = indexed[CALIBRATION]
    observed = comparison.spectrum(calibration, 3)
    wave = helpers.source_components(calibration)[0].amplitude
    coefficients = [0.0] * 12
    odd_fits = {}
    for order in ODD_ORDERS:
        label = comparison.label(comparison.traced.expression((10,) * order), 1)
        if label not in observed:
            raise ValueError("Missing isolated harmonic calibration")
        rows = helpers.pair(observed[label], order * 1e9, float(order))
        ratios = [row[1] / (5.0 ** (order - 1) * wave**order) for row in rows]
        if any(
            not math.isfinite(ratio.real)
            or ratio.real >= 0
            or abs(ratio.imag) > TOLERANCE * abs(ratio)
            for ratio in ratios
        ):
            raise ValueError("Calibration requires a finite negative real odd coefficient")
        if abs(ratios[0] / ratios[1] - 1) > TOLERANCE:
            raise ValueError("Calibration band edges disagree")
        coefficients[order] = sum(ratio.real for ratio in ratios) / 2
        odd_fits[order] = coefficients[order]

    even_definitions = []
    for order, offset in EVEN_OFFSETS_DB.items():
        first = order // 2 + 1
        even_definitions.append(
            helpers.TwoToneIntercept(
                first, first - order, 20 + offset, 1, helpers.InterceptReference.OUTPUT
            )
        )
    even = library.polynomial_coefficients_from_intercepts(10, even_definitions)
    for order in EVEN_OFFSETS_DB:
        coefficients[order] = even[order]

    cases = []
    aggregate = {order: [] for order in range(4, 12)}
    for key, capture in sorted(indexed.items()):
        sources = helpers.source_components(capture)
        drive = math.fsum(abs(source.amplitude) ** 2 for source in sources)
        point = library.amplifier_operating_point(
            drive,
            power_gain_db=10,
            output_p1db_dbm=20,
            output_saturation_dbm=23,
            input_ip2_dbm=30,
            input_ip3_dbm=20,
        )
        scaled = [
            source._replace(amplitude=source.amplitude * point.nonlinear_input_scale)
            for source in sources
        ]
        response = library.coherent_polynomial(
            1e8, scaled, coefficients[: capture["maximum_order"] + 1]
        )
        observed = comparison.spectrum(capture, 3)
        selected, checks, missing = set(), [], []
        for term in response.terms:
            label = term_label(term, response.inputs)
            if label in selected:
                raise ValueError("Duplicate predicted origin")
            selected.add(label)
            if label not in observed:
                missing.append({"order": term.order, "label": label, "bin": term.component.bin})
                continue
            rows = helpers.pair(
                observed[label], term.component.bin * 1e8, term.component.bandwidth_hz
            )
            error = max(abs(row[1] / term.component.amplitude - 1) for row in rows)
            check = {
                "order": term.order,
                "label": label,
                "bin": term.component.bin,
                "expected_wave": [term.component.amplitude.real, term.component.amplitude.imag],
                "observed_waves": [[row[1].real, row[1].imag] for row in rows],
                "relative_wave_error": error,
                "passed": error <= TOLERANCE,
                "unscaled_relative_wave_error": max(
                    abs(
                        row[1]
                        / (term.component.amplitude / point.nonlinear_input_scale**term.order)
                        - 1
                    )
                    for row in rows
                ),
            }
            checks.append(check)
            # The entire odd calibration capture is excluded from holdout scoring.
            if key != CALIBRATION or term.order not in ODD_ORDERS:
                aggregate[term.order].append(check)
        for label in observed:
            if label.startswith("[") and label.endswith("],RFAmp1"):
                counts = re.findall(r"(?:(\d+)x)?\(Source\.Source[12]\)", label)
                order = sum(int(count) if count else 1 for count in counts)
                if order >= 4 and label not in selected:
                    raise ValueError("Unexpected observed high-order origin")
        # Equal high-power captures and selector controls must be complete.
        if capture["source_power_dbm"] == 5 and capture["second_source_power_dbm"] == 5 and missing:
            raise ValueError("Incomplete required high-power origin coverage")
        cases.append(
            {
                "configuration": dict(
                    zip(
                        (
                            "maximum_order",
                            "source_power_dbm",
                            "source_phase_deg",
                            "two_tone",
                            "second_source_power_dbm",
                        ),
                        key,
                    )
                ),
                "odd_role": "calibration" if key == CALIBRATION else "holdout",
                "operating_point": {
                    "nominal_input_power_w": drive,
                    "nonlinear_input_scale": point.nonlinear_input_scale,
                    "limited_input_power_w": point.limited_input_power_w,
                },
                "orders": {
                    str(order): helpers.summarize([c for c in checks if c["order"] == order])
                    for order in range(4, capture["maximum_order"] + 1)
                },
                "all_predicted_origins_recorded": not missing,
                "not_recorded": missing,
                "checks": checks,
                "raw_capture_sha256": capture["raw_capture_sha256"],
            }
        )
    return {
        "classification": "diagnostic_only",
        "eligible_for_compatibility": False,
        "scope": "First-stage generated orders 4..11 with spectrum reduction disabled",
        "relative_wave_tolerance": TOLERANCE,
        "calibration_configuration": list(CALIBRATION),
        "even_rules": {
            str(order): {
                "output_intercept_dbm": 20 + offset,
                "reference_product": [order // 2 + 1, 1 - order // 2],
                "voltage_coefficient": coefficients[order],
                **helpers.summarize(aggregate[order]),
            }
            for order, offset in EVEN_OFFSETS_DB.items()
        },
        "odd_hypotheses": {
            str(order): {
                "effective_voltage_coefficient": odd_fits[order],
                **helpers.summarize(aggregate[order]),
            }
            for order in ODD_ORDERS
        },
        "cases": cases,
        "limitations": [
            "Odd coefficients are identified at one RFAMP parameter configuration using nominal source waves.",
            "No automatic RFAMP odd coefficient synthesis is implemented by this diagnostic.",
            "Nominal even-coefficient residuals retain the original 1e-7 tolerance; failures remain failures.",
            "Missing low-power terms are unrecorded, not zero and not numerical passes.",
            "Existing common nonlinear input limiting is evaluated, but not full RFAMP compression or total spectra.",
            "No second-stage or noise compatibility acceptance.",
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
        if all(
            summary["consistent"]
            for family in ("even_rules", "odd_hypotheses")
            for summary in report[family].values()
        )
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
