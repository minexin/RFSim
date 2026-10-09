"""Compare a controlled two-source TLE/tee/attenuator network with SystemVue."""
import argparse
import cmath
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from rfmodel import Library, CoherentComponent, PortCoherentComponent, SpectrumKind, SourceCoherence

spec = importlib.util.spec_from_file_location("runner", Path(__file__).with_name("run-systemvue-reference.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
BASE = "RFModel_PhaseCombiner/Phase Prj/"
VARS = BASE + "System1_Data/Eqns/VarBlock/"


def number(value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError("Expected finite numeric measurement")
    return value


def inspect(capture):
    runner.validate_capture(capture, "coherent")
    if type(capture["locked"]) is not bool:
        raise ValueError("Expected explicit locked flag")
    phase = number(capture["second_phase_deg"])
    length = number(capture["line_length_rad"])
    if not -360 <= phase <= 360 or not 0 <= length <= 100:
        raise ValueError("Phase or electrical length outside controlled range")
    nodes = {n["path"]: n for n in capture["nodes"]}
    if len(nodes) != len(capture["nodes"]):
        raise ValueError("Duplicate capture node")
    if capture.get("systemvue_version") != "2023.0.0.11903":
        raise ValueError("Unexpected reference version")
    if not re.fullmatch("[0-9a-f]{64}", capture["raw_capture_sha256"]):
        raise ValueError("Missing capture provenance")

    def parameter(part, name, expected):
        actual = nodes[BASE + "Example/PartList/" + part + "/ParamSet/" + name]["data"]
        if isinstance(expected, str):
            if actual != expected:
                raise ValueError("Unexpected text parameter: " + part + "/" + name)
        elif isinstance(expected, list):
            if actual != expected or any(type(x) is not int for x in actual):
                raise ValueError("Unexpected source mode: " + name)
        elif not math.isclose(number(actual), expected, rel_tol=1e-12, abs_tol=1e-15):
            raise ValueError("Unexpected parameter: " + part + "/" + name)

    for index in (1, 2):
        source = "MultiSource" + str(index)
        for name, value in {"Freq": 1e9, "Pwr": .001, "R": 50., "BW": 1e6,
                            "Phase": math.radians(phase) if index == 2 else 0.,
                            "Enable": 1, "SrcType": 0, "MultiCarrier": 0, "EnablePN": 0,
                            "Name": "Source" + str(index),
                            "RefClk": "RFModelClock" if capture["locked"] else ""}.items():
            parameter(source, name, value)
        for name, value in {"L": length, "F": 1e9, "Z": 50., "A": 0.}.items():
            parameter("TL" + str(index), name, value)
    for part, name, value in (("Attn1", "L", 10**.5), ("Attn1", "ZIN", 50.),
                               ("Attn1", "ZOUT", 50.), ("Port_3", "ZO", 50.)):
        parameter(part, name, value)

    def vector(name):
        node = nodes[VARS + name]
        values = node["data"]
        if not isinstance(values, list) or node["dimensions"] != [len(values)]:
            raise ValueError("Expected flat measurement vector: " + name)
        return values

    ids, names = vector("IDNo"), vector("IDName")
    if len(ids) != len(names) or len(set(ids)) != len(ids) or any(type(i) is not int for i in ids):
        raise ValueError("Invalid spectrum identity table")
    identity = dict(zip(ids, names))
    frequencies, powers, indices = vector("F3"), vector("P3"), vector("ID3")
    voltages, impedances = vector("V3"), vector("Z3")
    if len(frequencies) != len(powers) or len(indices) != len(powers) or len(indices) != 4:
        raise ValueError("Inconsistent output measurement lengths")
    if len(voltages) != 2 * len(indices) or len(impedances) != 2 * len(indices):
        raise ValueError("Invalid complex output encoding")
    waves, groups = {}, {}
    for source in (1, 2):
        suffix = "D[(MultiSource{0}.Source{0})],MultiSource{0},TL{0},Attn1".format(source)
        rows = []
        for i, identifier in enumerate(indices):
            if identifier not in identity:
                raise ValueError("Unknown output spectrum ID")
            match = re.fullmatch(r"\{([1-9][0-9]*)\}" + re.escape(suffix), identity[identifier])
            if match:
                rows.append((i, int(match.group(1))))
        if len(rows) != 2 or rows[0][1] != rows[1][1]:
            raise ValueError("Missing unique source CW boundaries")
        if sorted(number(frequencies[i]) for i, _ in rows) != [1e9 - .5, 1e9 + .5]:
            raise ValueError("Unexpected CW frequency or bandwidth")
        groups[source] = rows[0][1]
        waves[source] = []
        for i, _ in rows:
            v = complex(number(voltages[2*i]), number(voltages[2*i+1]))
            z = complex(number(impedances[2*i]), number(impedances[2*i+1]))
            if abs(z.imag) > 1e-7 or not math.isclose(z.real, 50., rel_tol=1e-7):
                raise ValueError("Output is not a matched 50-ohm observation")
            power = number(powers[i])
            if power <= 0 or not math.isclose(abs(v)**2 / (2*z.real), power, rel_tol=1e-10):
                raise ValueError("Output voltage/power mismatch")
            waves[source].append(v / math.sqrt(2*z.real))
    if (groups[1] == groups[2]) != capture["locked"]:
        raise ValueError("Measured coherency IDs disagree with source clocks")
    elements, drive = vector("RFElemList"), vector("RFPwrIn")
    if len(elements) != len(drive) or elements.count("Attn1") != 1:
        raise ValueError("Missing unique attenuator input drive")
    measured_drive = number(drive[elements.index("Attn1")])
    if measured_drive < 0:
        raise ValueError("Negative RF input drive")
    return phase, length, groups, waves, measured_drive


def predict(library, phase, length, groups, *, attenuator, selected_sources=(1, 2)):
    with library.network() as network:
        wave = cmath.exp(-1j * length)
        network.add([[0, wave], [wave, 0]])
        network.add([[0, wave], [wave, 0]])
        # Equal-reference ideal tee: diagonal -1/3, every through path +2/3.
        network.add([[-1/3, 2/3, 2/3], [2/3, -1/3, 2/3], [2/3, 2/3, -1/3]])
        network.connect(1, 4)
        network.connect(3, 5)
        output = 6
        if attenuator:
            transmission = 10**(-5/20)
            network.add([[0, transmission], [transmission, 0]])
            network.connect(6, 7)
            output = 8
        inputs = [
            PortCoherentComponent(0 if source == 1 else 2,
                CoherentComponent(10, SpectrumKind.SOURCE, 1., groups[source],
                    math.sqrt(.001) * cmath.exp(1j * math.radians(phase if source == 2 else 0.))))
            for source in selected_sources]
        return network.transmit_coherent(1e8, inputs, [output, 2, 0], output)


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Expected nonempty capture list")
    reports, seen = [], set()
    for capture in captures:
        phase, length, groups, waves, measured_drive = inspect(capture)
        parameters = {node["path"]: node["data"] for node in capture["nodes"]}
        definitions = [
            SourceCoherence("MultiSource" + str(index) + ".Source" + str(index),
                            parameters[BASE + "Example/PartList/MultiSource" + str(index) + "/ParamSet/RefClk"])
            for index in (1, 2)]
        assigned = library.assign_source_coherence(definitions)
        resolved = dict(zip((1, 2), assigned))
        same_relation = (resolved[1] == resolved[2]) == (groups[1] == groups[2])
        key = capture["locked"], phase, length
        if key in seen:
            raise ValueError("Duplicate reference configuration")
        seen.add(key)
        checks = []
        for source in (1, 2):
            result = predict(library, phase, length, resolved, attenuator=True, selected_sources=(source,))
            predicted = result.components[0].amplitude
            errors = [abs(predicted-observed) / abs(observed) for observed in waves[source]]
            checks.append({"source": source, "coherency_number": groups[source],
                           "rfmodel_coherence_group": resolved[source],
                           "rfmodel_wave": [predicted.real, predicted.imag],
                           "systemvue_boundary_waves": [[w.real, w.imag] for w in waves[source]],
                           "relative_complex_errors": errors, "passed": max(errors) <= 1e-7})
        input_drive = predict(library, phase, length, resolved, attenuator=False)
        # Use an absolute floor at cancellation; relative error alone is undefined at zero.
        tolerance = max(1e-15, measured_drive * 1e-7)
        delta = abs(input_drive.total_power_w - measured_drive)
        checks.append({"measurement": "RFPwrIn(Attn1)", "rfmodel_w": input_drive.total_power_w,
                       "systemvue_w": measured_drive, "absolute_error_w": delta,
                       "tolerance_w": tolerance, "passed": delta <= tolerance})
        checks.append({"measurement": "source_clock_coherence_relation",
                       "systemvue_groups": [groups[1], groups[2]], "rfmodel_groups": list(assigned),
                       "passed": same_relation})
        reports.append({"locked": capture["locked"], "second_phase_deg": phase,
                        "line_length_rad": length, "raw_capture_sha256": capture["raw_capture_sha256"],
                        "checks": checks})
    return {"scope": "Two-source TLE/tee/attenuator: path waves, clock relations and a separate RFPwrIn comparison",
            "relative_tolerance": 1e-7, "cancellation_absolute_tolerance_w": 1e-15,
            "coherency_assignment": "Native source/reference-clock resolver; compare relations, not numeric IDs",
            "path_wave_and_clock_relation_passed": all(
                c["passed"] for r in reports for c in (r["checks"][0], r["checks"][1], r["checks"][3])),
            "rfpwrin_agreement_passed": all(r["checks"][2]["passed"] for r in reports),
            "limitation": "This reference does not validate nonlinear/LO propagation or a split-source fixture; "
                          "RFPwrIn is a separately compared measurement, not assumed equivalent to coherent drive",
            "reports": reports, "passed": all(c["passed"] for r in reports for c in r["checks"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
