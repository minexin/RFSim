"""Compare coherent shared compression against direct RFAMP carrier spectra only."""
import argparse
import cmath
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location("two_tone", Path(__file__).with_name("compare-two-tone.py"))
two = importlib.util.module_from_spec(spec)
spec.loader.exec_module(two)
Library = two.single.Library
from rfmodel import CoherentComponent, SourceCoherence, SpectrumKind


def finite(value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError("Expected finite number")
    return value


def inspect(capture):
    for key in ("locked", "same_frequency", "diagnostic", "noise_enabled"):
        if type(capture[key]) is not bool:
            raise ValueError("Expected explicit boolean: " + key)
    if capture["systemvue_version"] != "2023.0.0.11903":
        raise ValueError("Unexpected reference version")
    for key in ("raw_capture_sha256", "source_workspace_sha256"):
        if not re.fullmatch("[0-9a-f]{64}", capture[key]):
            raise ValueError("Missing capture provenance")
    if capture.get("script_language") != "VBScript" or not isinstance(capture.get("submitted_script"), str):
        raise ValueError("Missing submitted script provenance")
    nodes = capture["nodes"]
    if len({n["path"] for n in nodes}) != len(nodes):
        raise ValueError("Duplicate capture nodes")
    powers, phases = capture["source_powers_dbm"], capture["source_phases_deg"]
    if not isinstance(powers, list) or len(powers) != 2:
        raise ValueError("Exactly two source powers required")
    two.inspect(capture, powers[0], second_power_dbm=powers[1], phases_deg=phases,
                same_frequency=capture["same_frequency"],
                cancellation_diagnostic=capture["diagnostic"])
    clocks = two.node(capture, "Sch1/PartList/Source/ParamSet/RefClk")
    expected = ["RFModelClock"] * 2 if capture["locked"] else ["", ""]
    if clocks["data"] != expected or clocks["dimensions"] != [2]:
        raise ValueError("Clock command disagrees with source readback")
    settings = capture["analysis_settings"]
    if type(settings.get("ShowTotals")) is not int or settings["ShowTotals"] != 1:
        raise ValueError("Expected enabled direction totals")
    if type(settings.get("CalcNoise")) is not int or settings["CalcNoise"] != int(capture["noise_enabled"]):
        raise ValueError("Noise command disagrees with analysis readback")
    if capture["diagnostic"] and (not capture["locked"] or not capture["same_frequency"] or
                                   not capture["manager_errors"]):
        raise ValueError("Expected a warned, same-frequency locked cancellation")
    ids, names = two.vector(capture, "IDNo"), two.vector(capture, "IDName")
    if (len(ids) != len(names) or len(set(ids)) != len(ids) or
            any(type(i) is not int for i in ids) or any(not isinstance(n, str) for n in names)):
        raise ValueError("Invalid spectrum identity map")
    identity = dict(zip(ids, names))
    output_paths = {two.BASE + two.SPECTRUM + k for k in ("F2", "P2", "ID2", "V2", "Z2")}
    present = output_paths.intersection(n["path"] for n in nodes)
    if not present and capture["diagnostic"] and not capture["noise_enabled"]:
        # SystemVue omits the entire output spectrum below its cutoff. This is
        # absence of a measurement, not a measured zero; keep it diagnostic.
        frequencies, observed, indices, voltage, impedance = [], [], [], [], []
    else:
        frequencies, observed, indices = (two.vector(capture, k) for k in ("F2", "P2", "ID2"))
        voltage, impedance = (two.vector(capture, k) for k in ("V2", "Z2"))
    count = len(indices)
    if (len(frequencies) != count or len(observed) != count or
            len(voltage) != 2*count or len(impedance) != 2*count or
            any(type(i) is not int or i not in identity for i in indices)):
        raise ValueError("Invalid output spectrum vectors")
    direct = []
    for identifier, label in identity.items():
        match = re.fullmatch(r"\{([1-9][0-9]*)\}D\[\(Source\.Source([12])\)\],Source,RFAmp", label)
        if not match:
            continue
        rows = [i for i, value in enumerate(indices) if value == identifier]
        if not rows:
            continue
        source = int(match.group(2))
        center = 1e9 if capture["same_frequency"] or source == 1 else 1.1e9
        if len(rows) != 2 or sorted(finite(frequencies[i]) for i in rows) != [center-.5, center+.5]:
            raise ValueError("Unexpected direct CW boundaries")
        waves, values = [], []
        for i in rows:
            z = complex(finite(impedance[2*i]), finite(impedance[2*i+1]))
            v = complex(finite(voltage[2*i]), finite(voltage[2*i+1]))
            power = finite(observed[i])
            if abs(z.imag) > 1e-7 or not math.isclose(z.real, 50., rel_tol=1e-7):
                raise ValueError("Expected matched 50-ohm output")
            if power <= 0 or not math.isclose(abs(v)**2/(2*z.real), power, rel_tol=1e-10):
                raise ValueError("Voltage/power inconsistency")
            waves.append(v/math.sqrt(2*z.real))
            values.append(power)
        direct.append(dict(source=source, bin=round(center/1e8), group=int(match.group(1)),
                           waves=waves, powers=values))
    expected_count = 0 if capture["diagnostic"] else 1 if (
        capture["locked"] and capture["same_frequency"]) else 2
    if len(direct) != expected_count:
        raise ValueError("Unexpected direct carrier count")
    if expected_count == 2:
        if {d["source"] for d in direct} != {1, 2}:
            raise ValueError("Missing independent source identity")
        if (direct[0]["group"] == direct[1]["group"]) != capture["locked"]:
            raise ValueError("Direct carrier groups disagree with clocks")
    elements, drive = two.vector(capture, "RFElemList"), two.vector(capture, "RFPwrIn")
    if (elements not in (["RFAmp"], [r"RFAmp\RFAmpHO"]) or
            len(drive) != 1 or finite(drive[0]) < 0):
        raise ValueError("Expected isolated amplifier input-power measurement")
    return powers, phases, direct, drive[0]


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty capture list required")
    reports, diagnostics, seen = [], [], set()
    for capture in captures:
        powers, phases, direct, measured_drive = inspect(capture)
        key = (tuple(powers), tuple(phases), capture["locked"], capture["same_frequency"], capture["noise_enabled"])
        if key in seen:
            raise ValueError("Duplicate reference configuration")
        seen.add(key)
        groups = library.assign_source_coherence([
            SourceCoherence("Source" + str(i+1), "RFModelClock" if capture["locked"] else "")
            for i in range(2)])
        components = [
            CoherentComponent(10 if capture["same_frequency"] or i == 0 else 11,
                              SpectrumKind.SOURCE, 1., groups[i],
                              math.sqrt(10**((powers[i]-30)/10))*cmath.exp(1j*math.radians(phases[i])))
            for i in range(2)]
        predicted = library.compress_coherent_fundamentals(
            1e8, components, power_gain_db=20., output_p1db_dbm=20., output_saturation_dbm=23.)
        if predicted.input_power_w > 10**(-2.9)*(1+1e-12):
            raise ValueError("Reference acceptance is limited to input P1dB or below")
        common = dict(source_powers_dbm=powers, source_phases_deg=phases,
                      same_frequency=capture["same_frequency"], locked=capture["locked"],
                      noise_enabled=capture["noise_enabled"],
                      raw_capture_sha256=capture["raw_capture_sha256"])
        if capture["diagnostic"]:
            diagnostics.append(dict(common, classification="diagnostic_only",
                                    manager_errors=capture["manager_errors"],
                                    rfmodel_input_w=predicted.input_power_w,
                                    systemvue_rfpwrin_w=measured_drive, direct_carriers=len(direct),
                                    output_spectrum_present=any(
                                        n["path"] == two.BASE + two.SPECTRUM + "F2" for n in capture["nodes"]),
                                    affects_compatibility_verdict=False))
            continue
        tolerance = max(1e-15, measured_drive*1e-7)
        checks = [dict(measurement="RFPwrIn(RFAmp)", rfmodel_w=predicted.input_power_w,
                       systemvue_w=measured_drive, tolerance_w=tolerance,
                       passed=abs(predicted.input_power_w-measured_drive) <= tolerance)]
        for observed in direct:
            group = groups[observed["source"]-1]
            candidates = [c for c in predicted.output.components
                          if c.bin == observed["bin"] and c.coherence_group == group]
            if len(candidates) != 1:
                raise ValueError("Ambiguous native carrier identity")
            wave = candidates[0].amplitude
            errors = [abs(wave-v)/abs(v) for v in observed["waves"]]
            checks.append(dict(measurement="direct_carrier", source_label=observed["source"],
                               bin=observed["bin"], rfmodel_wave=[wave.real, wave.imag],
                               systemvue_boundary_waves=[[v.real, v.imag] for v in observed["waves"]],
                               relative_complex_errors=errors, passed=max(errors) <= 1e-7))
        reports.append(dict(common, checks=checks))
    if not reports:
        raise ValueError("At least one warning-free comparison case is required")
    return dict(scope="Isolated RFAMP direct-carrier shared compression, input at/below P1dB",
                relative_tolerance=1e-7, cancellation_absolute_tolerance_w=1e-15,
                warning_free_case_count=len(reports), diagnostic_case_count=len(diagnostics),
                direct_carrier_passed=all(c["passed"] for r in reports for c in r["checks"][1:]),
                drive_power_passed=all(r["checks"][0]["passed"] for r in reports),
                reports=reports, diagnostics=diagnostics,
                passed=all(c["passed"] for r in reports for c in r["checks"]),
                limitations=["Only direct carrier spectra are compared, not the full output total",
                             "Cancellation below the -200 dBm threshold remains diagnostic",
                             "No saturation, reverse-drive or multiport nonlinear fixture acceptance",
                             "PhaseCombiner Attn1 RFPwrIn discrepancy remains unresolved"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite input files")
    raw = args.captures.read_bytes()
    report = compare(Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
