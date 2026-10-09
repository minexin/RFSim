"""Compare individual local mixing paths, including carrier-overlapping cubic terms."""
import argparse
import cmath
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location("two_tone", Path(__file__).with_name("compare-two-tone.py"))
two_tone = importlib.util.module_from_spec(spec)
spec.loader.exec_module(two_tone)
EXPECTED = {
    (1, (10,)), (1, (11,)),
    (2, (-10, 11)), (2, (10, 10)), (2, (10, 11)), (2, (11, 11)),
    (3, (-11, 10, 10)), (3, (-11, 10, 11)), (3, (-10, 10, 10)),
    (3, (-11, 11, 11)), (3, (-10, 10, 11)), (3, (-10, 11, 11)),
    (3, (10, 10, 10)), (3, (10, 10, 11)), (3, (10, 11, 11)), (3, (11, 11, 11)),
}


def expression(contributors):
    pieces = []
    for index, count in sorted(Counter(contributors).items()):
        source = {10: "Source1", 11: "Source2"}[abs(index)]
        piece = (str(count) + "x" if count > 1 else "") + "(Source." + source + ")"
        pieces.append(("-" if index < 0 else "+" if pieces else "") + piece)
    return "".join(pieces)


def source_powers(capture):
    if "source_powers_dbm" in capture:
        if "source_power_dbm_per_tone" in capture:
            raise ValueError("Ambiguous source power metadata")
        pair = capture["source_powers_dbm"]
    elif "source_power_dbm_per_tone" in capture:
        pair = [capture["source_power_dbm_per_tone"]] * 2
    else:
        raise ValueError("Missing ordered source powers")
    if not isinstance(pair, list) or len(pair) != 2:
        raise ValueError("Exactly two ordered source powers required")
    if any(isinstance(p, bool) or not isinstance(p, (int, float)) or not math.isfinite(p) for p in pair):
        raise ValueError("Source powers must be finite numbers")
    return tuple(pair)


def voltage_observer(capture):
    """Decode the collector's interleaved real/imaginary node voltage vectors."""
    ids = two_tone.vector(capture, "ID2")
    names = dict(zip(two_tone.vector(capture, "IDNo"), two_tone.vector(capture, "IDName")))

    def complex_vector(name):
        data = two_tone.vector(capture, name)
        if len(data) != 2 * len(ids) or any(
                isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v)
                for v in data):
            raise ValueError("Malformed interleaved complex vector: " + name)
        return [complex(data[i], data[i + 1]) for i in range(0, len(data), 2)]

    voltages, impedances = complex_vector("V2"), complex_vector("Z2")

    def observed(identity, power_w):
        indices = [i for i, identifier in enumerate(ids) if names[identifier] == identity]
        if len(indices) != 2:
            raise ValueError("Complex voltage must cover both CW boundaries")
        waves = []
        for i in indices:
            voltage, impedance = voltages[i], impedances[i]
            if impedance.imag != 0 or not math.isclose(impedance.real, 50, rel_tol=1e-7):
                raise ValueError("Complex reference comparison requires a real 50 ohm load")
            power = abs(voltage) ** 2 / (2 * impedance.real)
            if not math.isfinite(power) or not math.isclose(power, power_w, rel_tol=1e-10):
                raise ValueError("Peak voltage, impedance and power disagree")
            waves.append(voltage / math.sqrt(2 * impedance.real))
        if abs(waves[0] - waves[1]) > 1e-12 * abs(waves[0]):
            raise ValueError("Non-flat complex CW spectrum")
        return waves[0], voltages[indices[0]], impedances[indices[0]]

    return observed


def compare(library, captures, *, complex_amplitudes=False):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty controlled sweep required")
    reports, seen = [], set()
    for capture in captures:
        powers_dbm = source_powers(capture)
        profile = capture.get("profile", "sample")
        phases_deg = capture.get("source_phases_deg", [0, 0])
        power, parameters = two_tone.inspect(
            capture, powers_dbm[0], second_power_dbm=powers_dbm[1],
            profile=profile, phases_deg=phases_deg)
        identity = (profile, *powers_dbm, *phases_deg)
        if identity in seen:
            raise ValueError("Duplicate profile, ordered powers and phases")
        seen.add(identity)
        second_power = 10 ** ((powers_dbm[1] - 30) / 10)
        observed = two_tone.spectrum_observer(capture)
        observed_voltage = voltage_observer(capture) if complex_amplitudes else None
        verified = {p["parameter"]: p["value"] for p in parameters}
        gain_db = 10 * math.log10(verified["RFAmp/G"])
        traced = library.multitone_amplifier_terms(
            1e8, {10: cmath.rect(math.sqrt(power), math.radians(phases_deg[0])),
                  11: cmath.rect(math.sqrt(second_power), math.radians(phases_deg[1]))},
            power_gain_db=gain_db,
            output_p1db_dbm=30 + 10 * math.log10(verified["RFAmp/OP1dB"]),
            output_saturation_dbm=30 + 10 * math.log10(verified["RFAmp/OPSAT"]),
            input_ip2_dbm=30 + 10 * math.log10(verified["RFAmp/OIP2"]) - gain_db,
            input_ip3_dbm=30 + 10 * math.log10(verified["RFAmp/OIP3"]) - gain_db)
        keys = [(term.order, term.contributors) for term in traced.terms]
        if len(keys) != len(EXPECTED) or set(keys) != EXPECTED:
            raise ValueError("Incomplete or duplicate native mixing identities")
        checks = []
        for term in traced.terms:
            if sum(term.contributors) != term.bin:
                raise ValueError("Native contributors do not match output frequency")
            measured, identity, bounds = observed(expression(term.contributors), term.bin,
                                                  term.order, direct=term.order == 1)
            predicted = abs(term.amplitude) ** 2
            error = predicted / measured - 1
            checks.append({"order": term.order, "bin": term.bin, "contributors": list(term.contributors),
                           "identity": identity, "frequency_bounds_hz": bounds,
                           "carrier_overlap": term.order == 3 and term.bin in (10, 11),
                           "systemvue_power_w": measured, "rfmodel_power_w": predicted,
                           "signed_relative_error": error, "passed": abs(error) <= 1e-7})
            if observed_voltage is not None:
                wave, peak_voltage, impedance = observed_voltage(identity, measured)
                complex_error = abs(term.amplitude - wave) / abs(wave)
                checks[-1].update(
                    systemvue_amplitude=[wave.real, wave.imag],
                    rfmodel_amplitude=[term.amplitude.real, term.amplitude.imag],
                    peak_voltage=[peak_voltage.real, peak_voltage.imag],
                    impedance_ohms=[impedance.real, impedance.imag],
                    coherency_number=int(identity.split("}")[0][1:]),
                    complex_relative_error=complex_error,
                    phase_error_deg=math.degrees(cmath.phase(term.amplitude / wave)),
                    passed=checks[-1]["passed"] and complex_error <= 1e-7)
        reports.append({"source_phases_deg": list(phases_deg), "profile": profile,
                        "source_powers_dbm": list(powers_dbm), "parameters": parameters,
                        "raw_capture_sha256": capture["raw_capture_sha256"],
                        "run_started_utc": capture["run_started_utc"],
                        "manager_messages": capture["manager_errors"], "checks": checks})
    return {"scope": "Sixteen separately identified direct/second/third-order RF terms per controlled two-tone case",
            "comparison": "complex_amplitude_and_power" if complex_amplitudes else "power_only",
            "limitation": "Individual terms only; no coherent carrier sums or multi-stage provenance",
            "relative_tolerance": 1e-7, "reports": reports,
            "passed": all(c["passed"] for r in reports for c in r["checks"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--complex-amplitudes", action="store_true",
                        help="Also compare phase and amplitude using measured peak voltage and impedance")
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(two_tone.single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")),
                     complex_amplitudes=args.complex_amplitudes)
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
