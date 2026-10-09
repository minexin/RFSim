"""Separate nominal-source error from high-order local voltage response."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "input_reference_diagnostic", Path(__file__).with_name("diagnose-eleventh-order.py")
)
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
comparison = base.comparison


def measured_sources(capture, reference):
    if reference.get("source_port") != 1:
        raise ValueError("Expected first-stage input port")
    reference_ohms = comparison.finite(reference["reference_ohms"])
    if reference_ohms != comparison.value(capture, "Design3/PartList/RFAmp1/ParamSet/Zref"):
        raise ValueError("Input normalization reference differs from model reference")
    nominal = base.helpers.source_components(capture)
    sources = reference["sources"]
    if not isinstance(sources, list) or len(sources) != len(nominal):
        raise ValueError("Missing or extra measured source")
    converted, observations = [], []
    for index, (source, definition) in enumerate(zip(nominal, sources), 1):
        if (
            definition.get("source_index") != index
            or definition.get("label") != f"D[(Source.Source{index})],Source"
        ):
            raise ValueError("Measured source identity mismatch")
        samples = definition["samples"]
        if not isinstance(samples, list) or len(samples) != 2:
            raise ValueError("Expected two input band-edge samples")
        samples = sorted(samples, key=lambda row: row["frequency_hz"])
        waves, delivered = [], []
        for row, edge in zip(samples, (-0.5, 0.5)):
            frequency = comparison.finite(row["frequency_hz"])
            power = comparison.finite(row["power_w"])
            for key in ("peak_voltage", "impedance_ohms"):
                if not isinstance(row[key], list) or len(row[key]) != 2:
                    raise ValueError("Expected encoded complex voltage/impedance")
                for value in row[key]:
                    comparison.finite(value)
            voltage = complex(*row["peak_voltage"])
            impedance = complex(*row["impedance_ohms"])
            if (
                abs(frequency - (source.bin * 1e8 + edge)) > 1e-4
                or power <= 0
                or abs(impedance.imag) > 1e-7
                or not math.isclose(impedance.real, 50.0, rel_tol=1e-7)
                or not math.isclose(abs(voltage) ** 2 / (2 * impedance.real), power, rel_tol=1e-10)
            ):
                raise ValueError("Input frequency, voltage, impedance or power disagreement")
            waves.append(voltage / math.sqrt(2 * reference_ohms))
            delivered.append(power)
        if abs(waves[0] / waves[1] - 1) > 1e-12:
            raise ValueError("Input band-edge waves disagree")
        wave = sum(waves) / 2
        converted.append(source._replace(amplitude=wave))
        observations.append(
            {
                "source_index": index,
                "nominal_wave": [source.amplitude.real, source.amplitude.imag],
                "measured_voltage_wave": [wave.real, wave.imag],
                "relative_to_nominal": [
                    (wave / source.amplitude).real,
                    (wave / source.amplitude).imag,
                ],
                "fixed_reference_power_w": abs(wave) ** 2,
                "delivered_power_w": sum(delivered) / 2,
            }
        )
    delivered = math.fsum(row["delivered_power_w"] for row in observations)
    native_drive = comparison.vector(capture, "RFPwrIn")[0]
    if not math.isclose(delivered, native_drive, rel_tol=1e-12, abs_tol=0.0):
        raise ValueError("Measured source delivery disagrees with captured input power")
    return converted, {
        "sources": observations,
        "fixed_reference_drive_w": math.fsum(abs(source.amplitude) ** 2 for source in converted),
        "delivered_drive_w": delivered,
        "native_input_power_w": native_drive,
    }


def diagnose(library, captures, references):
    if not isinstance(references, list):
        raise ValueError("Measured input reference list required")
    indexed = {}
    for reference in references:
        digest = reference["raw_capture_sha256"]
        if digest in indexed:
            raise ValueError("Duplicate measured input reference")
        indexed[digest] = reference
    if set(indexed) != {capture["raw_capture_sha256"] for capture in captures}:
        raise ValueError("Input references must match the exact raw captures")
    decoded, observations = {}, {}
    for capture in captures:
        comparison.inspect(capture)
        digest = capture["raw_capture_sha256"]
        decoded[digest], observations[digest] = measured_sources(capture, indexed[digest])
    nominal = base.diagnose(library, captures)
    actual = base.diagnose(
        library, captures, source_provider=lambda capture: decoded[capture["raw_capture_sha256"]]
    )
    actual["scope"] = (
        "First-stage local orders 4..11 driven by measured input voltage at fixed Zref"
    )
    actual["input_normalization"] = (
        "wave = measured peak V1 / sqrt(2 * model Zref); drive = sum(abs(wave)^2)"
    )
    actual["nominal_source_baseline"] = {
        family: nominal[family] for family in ("even_rules", "odd_hypotheses")
    }
    for case in actual["cases"]:
        case["input_reference"] = observations[case["raw_capture_sha256"]]
        case["operating_point"]["fixed_reference_input_power_w"] = case["operating_point"].pop(
            "nominal_input_power_w"
        )
    actual["limitations"] = [
        "Measured input voltage is an externally supplied reference, not a solved RFModel network input.",
        "Nominal-source comparison failures are retained; no production model coefficient was changed.",
        "Odd coefficients remain fitted at one parameter configuration, with calibration excluded from scoring.",
        "Delivered P1 is validated against RFPwrIn but is not substituted for fixed-reference polynomial drive.",
        "Missing low-power terms remain unrecorded; no total spectrum, second-stage or noise acceptance.",
    ]
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("input_references", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    inputs = (args.library, args.captures, args.input_references)
    if args.output.resolve() in {path.resolve() for path in inputs}:
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    references = args.input_references.read_bytes()
    report = diagnose(
        comparison.traced.two_tone.single.Library(args.library.resolve()),
        json.loads(raw.decode("utf-8-sig")),
        json.loads(references.decode("utf-8-sig")),
    )
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["input_references_sha256"] = hashlib.sha256(references).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return (
        0
        if all(
            s["consistent"]
            for family in ("even_rules", "odd_hypotheses")
            for s in report[family].values()
        )
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
