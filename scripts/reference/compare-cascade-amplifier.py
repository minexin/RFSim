"""Validate controlled two-amplifier captures while preserving observed parameter-dependent gaps."""

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


runner = module("cascade_runner", "run-systemvue-reference.py")
traced = module("cascade_terms", "compare-amplifier-terms.py")
from rfmodel import CoherentComponent, SpectrumKind
from rfmodel.coherent_system_file import analyze_coherent_system

ROOT = "RFModel_CascadeIntermods/Designs/"
SPECTRUM = "System3_Design3_Data/Eqns/VarBlock/"
SETTINGS = {
    "AutoRecalc": 0,
    "CalcHarmonics": 1,
    "CalcIntermods": 1,
    "CalcNoise": 0,
    "CalcPhaseNoise": 0,
    "CoherentIM": 1,
    "ShowTotals": 1,
    "UseSourcePts": 0,
    "NeedRun": 0,
}
TOPOLOGY = {
    "Source": ("MultiSource", "1,1?Part0"),
    "RFAmp1": ("RFAMP", "Port_1,1?Part1|Port_2,3?Part3"),
    "RFAmp2": ("RFAMP", "Port_1,3?Part0|Port_2,2?Part2"),
    "Port_2": ("*OUT", "Port_1,2?Part3"),
}
META = (
    "source_power_dbm",
    "source_phase_deg",
    "maximum_order",
    "second_gain_db",
    "reverse_isolation_db",
    "channel_bandwidth_hz",
    "two_tone",
    "secondary_spectrum",
    "secondary_range_db",
)


def node(capture, suffix):
    rows = [n for n in capture["nodes"] if n["path"] == ROOT + suffix]
    if len(rows) != 1:
        raise ValueError("Missing/duplicate node: " + suffix)
    n = rows[0]
    if n.get("evaluation_error"):
        raise ValueError("Parameter evaluation failed")
    return n


def value(capture, suffix):
    return node(capture, suffix)["data"]


def vector(capture, name):
    n = node(capture, SPECTRUM + name)
    values, dimensions = n["data"], n["dimensions"]
    if (
        not isinstance(values, list)
        or not isinstance(dimensions, list)
        or any(type(d) is not int or d < 0 for d in dimensions)
        or math.prod(dimensions) != len(values)
    ):
        raise ValueError("Malformed spectrum array: " + name)
    return values


def finite(x):
    if type(x) not in (int, float) or not math.isfinite(x):
        raise ValueError("Expected finite scalar")
    return x


def equal(actual, expected):
    if isinstance(expected, list):
        if not isinstance(actual, list):
            actual = [actual]
        return len(actual) == len(expected) and all(equal(a, e) for a, e in zip(actual, expected))
    if isinstance(expected, str):
        return actual == expected
    return math.isclose(finite(actual), expected, rel_tol=1e-12, abs_tol=0.0)


def inspect(capture):
    runner.validate_capture(capture, "cascade")
    # Independently require a fresh main spectrum dataset as well as the path dataset.
    mirror = dict(
        capture,
        nodes=[
            dict(
                n,
                path=(
                    (ROOT + "System3_Data_Folder/System3_Design3_Data_Path1")
                    if n["path"] == ROOT + "System3_Design3_Data"
                    else n["path"]
                ),
            )
            for n in capture["nodes"]
            if n["path"] != ROOT + "System3_Data_Folder/System3_Design3_Data_Path1"
        ],
    )
    runner.validate_capture(mirror, "cascade")
    if capture["systemvue_version"] != "2023.0.0.11903" or capture["script_language"] != "VBScript":
        raise ValueError("Unexpected reference version or script language")
    if not isinstance(capture["submitted_script"], str) or not capture[
        "submitted_script"
    ].startswith("Dim "):
        raise ValueError("Missing submitted script")
    for key in ("raw_capture_sha256", "workspace_sha256"):
        if not re.fullmatch("[0-9a-f]{64}", capture[key]):
            raise ValueError("Missing provenance digest")
    for key in ("two_tone", "secondary_spectrum"):
        if type(capture[key]) is not bool:
            raise ValueError("Expected explicit boolean metadata")
    power, phase = finite(capture["source_power_dbm"]), finite(capture["source_phase_deg"])
    gain, riso = finite(capture["second_gain_db"]), finite(capture["reverse_isolation_db"])
    bandwidth = finite(capture["channel_bandwidth_hz"])
    if (
        not -60 <= power <= -10
        or not -180 <= phase <= 180
        or not -10 <= gain <= 20
        or riso not in (100, 140)
        or bandwidth not in (1, 1e6)
        or type(capture["maximum_order"]) is not int
        or capture["maximum_order"] not in (3, 5)
        or type(capture["secondary_range_db"]) is not int
        or capture["secondary_range_db"] not in (-140, -50, 50, 140)
    ):
        raise ValueError("Uncontrolled cascade settings")
    settings = node(capture, "System3")["analysis_settings"]
    if any(type(settings.get(k)) is not int or settings[k] != v for k, v in SETTINGS.items()):
        raise ValueError("Unexpected analysis readback")
    expected = {
        "System3/MaxOrder": capture["maximum_order"],
        "System3/ChanBW": bandwidth,
        "System3/UseVolterra": int(capture["secondary_spectrum"]),
        "System3/UseWithin": 10 ** (capture["secondary_range_db"] / 10),
        "System3/Path0/PathFreq": 1e9,
        "System3/DesignName": "Design3",
        "Design3/PartList/Port_2/ParamSet/ZO": 50.0,
    }
    for name, (model, netlist) in TOPOLOGY.items():
        n = node(capture, "Design3/PartList/" + name)
        if n["model"] != model or n["netlist"] != netlist:
            raise ValueError("Unexpected native circuit topology")
    for index in (1, 2):
        parameters = {
            "G": 10 ** ((10 if index == 1 else gain) / 10),
            "NF": 10**0.3,
            "OP1dB": 0.1,
            "OPSAT": 10 ** (-0.7),
            "OIP2": 10.0,
            "OIP3": 1.0,
            "RISO": 10 ** (riso / 10),
            "ZIN": 50.0,
            "ZOUT": 50.0,
            "Zref": 50.0,
            "PortParamType": 0,
            "AMtoPM_Mode": 0,
            "EnablePN": 0,
            "FrequencyMode": 0,
        }
        expected.update(
            {f"Design3/PartList/RFAmp{index}/ParamSet/{k}": v for k, v in parameters.items()}
        )
    tones = 2 if capture["two_tone"] else 1
    source = {
        "Name": ["Source1", "Source2"][:tones],
        "Enable": [1] * tones,
        "SrcType": [0] * tones,
        "MultiCarrier": [0] * tones,
        "EnablePN": [0] * tones,
        "Freq": [1e9, 1.1e9][:tones],
        "BW": 1.0,
        "Pwr": [10 ** ((power - 30) / 10)] * tones,
        "Phase": [0.0, math.radians(phase)] if tones == 2 else [math.radians(phase)],
        "RefClk": [""] * tones,
        "R": 50.0,
        "ZO": 50.0,
    }
    expected.update({"Design3/PartList/Source/ParamSet/" + k: v for k, v in source.items()})
    for suffix, expected_value in expected.items():
        if not equal(value(capture, suffix), expected_value):
            raise ValueError("Parameter mismatch: " + suffix)
    elements = vector(capture, "RFElemList")
    if elements != ["RFAmp1\\RFAmpHO", "RFAmp2\\RFAmpHO"]:
        raise ValueError("Unexpected RF element order")
    drive = vector(capture, "RFPwrIn")
    if len(drive) != 2 or any(finite(v) < 0 for v in drive):
        raise ValueError("Invalid input power array")
    return tones, expected


def spectrum(capture, port):
    numbers, names = vector(capture, "IDNo"), vector(capture, "IDName")
    if (
        len(numbers) != len(names)
        or len(set(numbers)) != len(numbers)
        or any(type(i) is not int for i in numbers)
        or any(not isinstance(n, str) for n in names)
    ):
        raise ValueError("Invalid identity map")
    identities = dict(zip(numbers, names))
    fs, ps, ids, vs, zs = [vector(capture, k + str(port)) for k in ("F", "P", "ID", "V", "Z")]
    if not len(fs) == len(ps) == len(ids) or len(vs) != 2 * len(ids) or len(zs) != len(vs):
        raise ValueError("Invalid spectrum vector sizes")
    records = {}
    for i, identifier in enumerate(ids):
        if type(identifier) is not int or identifier not in identities:
            raise ValueError("Invalid spectrum identity")
        label = identities[identifier]
        match = re.fullmatch(r"\{([1-9][0-9]*)\}(D?\[.*\],.*)", label)
        if not match:
            if label.startswith("{") and not label.startswith("{0}"):
                raise ValueError("Unknown RF identity syntax")
            continue
        z = complex(finite(zs[2 * i]), finite(zs[2 * i + 1]))
        peak = complex(finite(vs[2 * i]), finite(vs[2 * i + 1]))
        power = finite(ps[i])
        if (
            abs(z.imag) > 1e-7
            or not math.isclose(z.real, 50.0, rel_tol=1e-7)
            or power <= 0
            or not math.isclose(abs(peak) ** 2 / (2 * z.real), power, rel_tol=1e-10)
        ):
            raise ValueError("Voltage, impedance and power disagree")
        records.setdefault(match.group(2), []).append(
            (finite(fs[i]), peak / math.sqrt(2 * z.real), int(match.group(1)))
        )
    return records


def key(c):
    return c.bin, c.kind, c.bandwidth_hz, c.coherence_group


def label(expression, stage, *, direct=False, conducted=False):
    return (
        ("D" if direct else "")
        + "["
        + expression
        + "],"
        + ("Source," if direct else "")
        + ("RFAmp1" if stage == 1 else "RFAmp1,RFAmp2" if conducted else "RFAmp2")
    )


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty capture list required")
    reports, seen = [], set()
    for capture in captures:
        tones, parameters = inspect(capture)
        if capture["maximum_order"] != 3:
            raise ValueError("Cubic comparison requires maximum_order=3")
        configuration = tuple(capture[k] for k in META)
        if configuration in seen:
            raise ValueError("Duplicate configuration")
        seen.add(configuration)
        source = [
            CoherentComponent(
                10 + i,
                SpectrumKind.SOURCE,
                1.0,
                i + 1,
                cmath.rect(
                    math.sqrt(10 ** ((capture["source_power_dbm"] - 30) / 10)),
                    math.radians(capture["source_phase_deg"] if tones == 1 or i == 1 else 0),
                ),
            )
            for i in range(tones)
        ]
        common = dict(output_p1db_dbm=20.0, output_saturation_dbm=23.0)
        first = library.coherent_amplifier(
            1e8, source, power_gain_db=10.0, input_ip2_dbm=30.0, input_ip3_dbm=20.0, **common
        )
        gain = capture["second_gain_db"]
        second = library.coherent_amplifier(
            1e8,
            [t.component for t in first.terms],
            propagate_distortion=True,
            power_gain_db=gain,
            input_ip2_dbm=40 - gain,
            input_ip3_dbm=30 - gain,
            **common,
        )
        first_origins = {
            key(t.component): traced.expression(
                tuple((1 if i > 0 else -1) * (9 + abs(i)) for i in t.input_indices)
            )
            for t in first.terms
        }
        checks, grouped_waves, group_numbers, origin_groups = [], {}, {}, {}
        for stage, response, port in ((1, first, 3), (2, second, 2)):
            observed, used = spectrum(capture, port), set()
            for t in response.terms:
                if stage == 1:
                    expression = first_origins[key(t.component)]
                elif t.order == 1:
                    expression = first_origins[key(second.inputs[t.input_indices[0] - 1])]
                else:
                    expression = traced.expression(
                        tuple(
                            (1 if i > 0 else -1) * (9 + second.inputs[abs(i) - 1].coherence_group)
                            for i in t.input_indices
                        )
                    )
                direct = t.order == 1 and t.component.kind == SpectrumKind.SOURCE
                identity = label(expression, stage, direct=direct, conducted=t.order == 1)
                if identity in used:
                    raise ValueError("Duplicate native origin")
                used.add(identity)
                rows = observed.get(identity, [])
                c = t.component
                bounds = [c.bin * 1e8 - c.bandwidth_hz / 2, c.bin * 1e8 + c.bandwidth_hz / 2]
                if len(rows) != 2 or [r[0] for r in rows] != bounds or rows[0][2] != rows[1][2]:
                    raise ValueError("Missing or malformed origin spectrum: " + identity)
                scale = math.sqrt(min(1.0, capture["channel_bandwidth_hz"] / c.bandwidth_hz))
                wave = rows[0][1] / scale
                if abs(rows[1][1] / scale - wave) > 1e-12 * abs(wave):
                    raise ValueError("Non-flat CW spectrum")
                error = abs(c.amplitude - wave) / abs(wave)
                power_error = abs(c.amplitude) ** 2 / abs(wave) ** 2 - 1
                checks.append(
                    dict(
                        stage=stage,
                        identity=identity,
                        bandwidth_hz=c.bandwidth_hz,
                        systemvue_wave=[wave.real, wave.imag],
                        rfmodel_wave=[c.amplitude.real, c.amplitude.imag],
                        relative_complex_error=error,
                        relative_power_error=power_error,
                        passed=error <= 1e-7 and abs(power_error) <= 1e-7,
                    )
                )
                origin = (expression, c.bin, c.kind, c.bandwidth_hz)
                group_numbers.setdefault(origin, set()).add(rows[0][2])
                if stage == 1:
                    origin_groups[origin] = c.coherence_group
                else:
                    grouped_waves[origin] = grouped_waves.get(origin, 0j) + wave
            expected_observed = {
                identity
                for identity in observed
                if (identity.endswith(",RFAmp1") if stage == 1 else True)
            }
            if used != expected_observed:
                raise ValueError("Incomplete native RF spectrum")
        if any(len(groups) != 1 for groups in group_numbers.values()):
            raise ValueError("Measured inherited and generated origins are not coherent")
        if len({next(iter(g)) for g in group_numbers.values()}) != len(group_numbers):
            raise ValueError("Distinct measured origins unexpectedly share groups")
        model = dict(
            format="rfmodel.coherent-system",
            version=1,
            spacing_hz=1e8,
            sources=[dict(id="Source" + str(i + 1)) for i in range(tones)],
            inputs=[
                dict(
                    id="rf",
                    components=[
                        dict(
                            source="Source" + str(i + 1),
                            bin=c.bin,
                            bandwidth_hz=1.0,
                            amplitude=[c.amplitude.real, c.amplitude.imag],
                        )
                        for i, c in enumerate(source)
                    ],
                )
            ],
            stages=[
                dict(
                    id="first",
                    type="limited_amplifier",
                    input="rf",
                    output="first",
                    power_gain_db=10.0,
                    input_ip2_dbm=30.0,
                    input_ip3_dbm=20.0,
                    **common,
                ),
                dict(
                    id="second",
                    type="cascaded_amplifier",
                    input="first",
                    output="second",
                    power_gain_db=gain,
                    input_ip2_dbm=40 - gain,
                    input_ip3_dbm=30 - gain,
                    **common,
                ),
            ],
            outputs=["second"],
        )
        graph = analyze_coherent_system(library, model)
        output = next(s for s in graph["streams"] if s["id"] == "second")
        combined = {c["coherence_group"]: complex(*c["amplitude"]) for c in output["components"]}
        for origin, wave in grouped_waves.items():
            predicted = combined[origin_groups[origin]]
            error = abs(predicted - wave) / abs(wave)
            power_error = abs(predicted) ** 2 / abs(wave) ** 2 - 1
            checks.append(
                dict(
                    stage="combined",
                    origin=list(origin),
                    relative_complex_error=error,
                    relative_power_error=power_error,
                    passed=error <= 1e-7 and abs(power_error) <= 1e-7,
                )
            )
        for index, predicted in enumerate((first.total_input_power_w, second.total_input_power_w)):
            measured = vector(capture, "RFPwrIn")[index]
            error = abs(predicted / measured - 1)
            checks.append(
                dict(
                    stage=index + 1,
                    measurement="RFPwrIn",
                    systemvue_w=measured,
                    rfmodel_w=predicted,
                    relative_error=error,
                    passed=error <= 1e-7,
                )
            )
        reports.append(
            dict(
                configuration={k: capture[k] for k in META},
                raw_capture_sha256=capture["raw_capture_sha256"],
                parameters=parameters,
                checks=checks,
                passed=all(c["passed"] for c in checks),
            )
        )
    return dict(
        scope="Two matched RFAMP stages; primary-carrier distortion generation and conducted distortion",
        relative_tolerance=1e-7,
        reports=reports,
        limitations=[
            "No secondary distortion remixing verdict, even when UseVolterra readback is enabled",
            "Second-stage 0 dB gain discrepancy persists at 100 and 140 dB reverse isolation",
            "No DC, noise, AM/PM or complete SystemVue path-budget acceptance",
        ],
        passed=all(r["passed"] for r in reports),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite input")
    raw = args.captures.read_bytes()
    report = compare(
        traced.two_tone.single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig"))
    )
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
