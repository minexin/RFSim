"""Replay all deterministic RFAMP origins from controlled shared-compression captures."""
import argparse
import cmath
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


shared = module("shared", "compare-shared-compression.py")
traced = module("traced", "compare-amplifier-terms.py")
from rfmodel import CoherentComponent, SourceCoherence, SpectrumKind


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty controlled capture list required")
    reports, diagnostics, seen = [], [], set()
    for capture in captures:
        powers, phases, direct, measured_drive = shared.inspect(capture)
        key = (tuple(powers), tuple(phases), capture["locked"],
               capture["same_frequency"], capture["noise_enabled"])
        if key in seen:
            raise ValueError("Duplicate reference configuration")
        seen.add(key)
        if capture["diagnostic"]:
            diagnostics.append(dict(raw_capture_sha256=capture["raw_capture_sha256"],
                                    classification="diagnostic_only",
                                    manager_messages=capture["manager_errors"],
                                    affects_compatibility_verdict=False))
            continue
        groups = library.assign_source_coherence([
            SourceCoherence("Source" + str(i + 1), "RFModelClock" if capture["locked"] else "")
            for i in range(2)])
        inputs = [CoherentComponent(
            10 if capture["same_frequency"] or i == 0 else 11, SpectrumKind.SOURCE, 1., groups[i],
            cmath.rect(math.sqrt(10**((powers[i] - 30)/10)), math.radians(phases[i])))
            for i in range(2)]
        predicted = library.coherent_amplifier(
            1e8, inputs, power_gain_db=20., output_p1db_dbm=20., output_saturation_dbm=23.,
            input_ip2_dbm=20., input_ip3_dbm=10.)
        if predicted.total_input_power_w > 10**(-2.9)*(1 + 1e-12):
            raise ValueError("Acceptance limited to input P1dB or below")
        source_numbers = ([direct[0]["source"]] if capture["same_frequency"] and capture["locked"]
                          else [1, 2])
        if len(predicted.inputs) != len(source_numbers):
            raise ValueError("Native input partition disagrees with measured source identities")
        observed = shared.two.spectrum_observer(capture)
        voltage = traced.voltage_observer(capture)
        checks, used_labels, native_groups, measured_groups = [], [], [], []
        for term in predicted.terms:
            contributors = tuple((1 if i > 0 else -1) * (9 + source_numbers[abs(i) - 1])
                                 for i in term.input_indices)
            expression = traced.expression(contributors)
            c = term.component
            expected_kind = (SpectrumKind.SOURCE if term.order == 1 else
                             SpectrumKind.HARMONIC if len(set(term.input_indices)) == 1
                             and term.input_indices[0] > 0 else SpectrumKind.INTERMOD)
            if c.kind != expected_kind or c.coherence_group <= 0:
                raise ValueError("Native kind/group disagrees with the generating origin")
            measured, label, bounds = observed(expression, c.bin, term.order, direct=term.order == 1)
            if label in used_labels:
                raise ValueError("Duplicate native output identity")
            used_labels.append(label)
            if bounds[1] - bounds[0] != c.bandwidth_hz:
                raise ValueError("Native and measured bandwidth disagree")
            wave, peak, impedance = voltage(label, measured)
            relative_error = abs(c.amplitude - wave) / abs(wave)
            power_error = abs(c.amplitude)**2 / measured - 1
            native_groups.append(c.coherence_group)
            measured_groups.append(int(label.split("}")[0][1:]))
            checks.append(dict(order=term.order, input_indices=list(term.input_indices),
                               bin=c.bin, kind=c.kind.name, bandwidth_hz=c.bandwidth_hz,
                               identity=label, native_group=c.coherence_group,
                               systemvue_amplitude=[wave.real, wave.imag],
                               rfmodel_amplitude=[c.amplitude.real, c.amplitude.imag],
                               systemvue_power_w=measured, rfmodel_power_w=abs(c.amplitude)**2,
                               relative_complex_error=relative_error, relative_power_error=power_error,
                               passed=relative_error <= 1e-7 and abs(power_error) <= 1e-7))
        numbers = shared.two.vector(capture, "IDNo")
        labels = shared.two.vector(capture, "IDName")
        active = set(shared.two.vector(capture, "ID2"))
        rf_labels = {label for number, label in zip(numbers, labels) if number in active
                     and re.fullmatch(r"\{[1-9][0-9]*\}(?:D)?\[.*\],(?:Source,)?RFAmp", label)}
        if rf_labels != set(used_labels):
            raise ValueError("Missing or extra measured RF origins")
        groups_passed = all((a == b) == (x == y)
                            for a, x in zip(native_groups, measured_groups)
                            for b, y in zip(native_groups, measured_groups))
        reports.append(dict(source_powers_dbm=powers, source_phases_deg=phases,
                            same_frequency=capture["same_frequency"], locked=capture["locked"],
                            noise_enabled=capture["noise_enabled"],
                            raw_capture_sha256=capture["raw_capture_sha256"],
                            run_started_utc=capture["run_started_utc"],
                            systemvue_rfpwrin_w=measured_drive,
                            rfmodel_input_power_w=predicted.total_input_power_w,
                            group_partition_passed=groups_passed, checks=checks))
    if not reports:
        raise ValueError("At least one warning-free reference required")
    return dict(scope="Direct, second- and third-order source-resolved complex RF spectra",
                limitation="Single amplifier only; no noise-drive, total-spectrum or recursive nonlinear verdict",
                relative_tolerance=1e-7, reports=reports, diagnostics=diagnostics,
                passed=all(r["group_partition_passed"] and all(c["passed"] for c in r["checks"])
                           for r in reports))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(shared.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
