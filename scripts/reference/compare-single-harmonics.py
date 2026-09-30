"""Compare isolated RFAMP harmonic power samples, without integrating the P vector."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location("single", Path(__file__).with_name("compare-single-compression.py"))
single = importlib.util.module_from_spec(spec)
spec.loader.exec_module(single)


def compare(library, capture, source_power_dbm, *, output_saturation_dbm=None):
    verified = single.compare(library, capture, source_power_dbm,
                              output_saturation_dbm=output_saturation_dbm)
    base = single.BASE + "System1_Data/Eqns/VarBlock/"

    def vector(name):
        matches = [node for node in capture["nodes"] if node["path"] == base + name]
        if len(matches) != 1 or matches[0].get("evaluation_error"):
            raise ValueError("Missing/ambiguous spectrum vector: " + name)
        data = matches[0]["data"]
        if not isinstance(data, list) or matches[0]["dimensions"] != [len(data)]:
            raise ValueError("Invalid spectrum vector shape")
        return data

    identifiers, names = vector("IDNo"), vector("IDName")
    if len(identifiers) != len(names) or len(set(identifiers)) != len(identifiers):
        raise ValueError("Invalid spectrum identity map")
    frequencies, powers, ids = vector("F2"), vector("P2"), vector("ID2")
    if not len(frequencies) == len(powers) == len(ids):
        raise ValueError("Mismatched spectrum arrays")
    native = library.intercept_amplifier(
        1e9, {1: math.sqrt(10 ** ((source_power_dbm - 30) / 10))},
        power_gain_db=20, input_ip2_dbm=20, input_ip3_dbm=10)
    checks = []
    for harmonic in (2, 3):
        pattern = r"\{\d+\}\[" + str(harmonic) + r"x\(Source\.Source1\)\],RFAmp"
        matches = [(identifier, name) for identifier, name in zip(identifiers, names)
                   if isinstance(name, str) and re.fullmatch(pattern, name)]
        if len(matches) != 1:
            raise ValueError("Expected exactly one direct generated harmonic")
        identifier, name = matches[0]
        points = [(f, p) for f, p, i in zip(frequencies, powers, ids) if i == identifier]
        expected_frequencies = [harmonic * 1e9 - harmonic / 2., harmonic * 1e9 + harmonic / 2.]
        if len(points) != 2 or [point[0] for point in points] != expected_frequencies:
            raise ValueError("Expected the two boundaries of the 1 Hz tone's harmonic spectrum")
        if any(not isinstance(p, (float, int)) or not math.isfinite(p) or p <= 0 for _, p in points):
            raise ValueError("Invalid harmonic power sample")
        if not math.isclose(points[0][1], points[1][1], rel_tol=1e-12):
            raise ValueError("Harmonic spectrum is not flat")
        observed = points[0][1]
        predicted = abs(native[harmonic]) ** 2
        residual = predicted / observed - 1
        checks.append({"harmonic": harmonic, "identity": name, "spectrum_id": identifier,
                       "frequency_bounds_hz": expected_frequencies, "systemvue_power_w": observed,
                       "rfmodel_power_w": predicted, "signed_relative_error": residual,
                       "passed": abs(residual) <= 1e-7})
    # If H_n scales as P_in^n, this root is its conditional effective drive ratio.
    # It is inferred from measured harmonics, never fed back into the native prediction.
    ratios = [(check["systemvue_power_w"] / check["rfmodel_power_w"]) ** (1 / check["harmonic"])
              for check in checks]
    return {
        "scope": "Direct generated H2/H3 power samples of the isolated sample-profile RFAMP",
        "limitation": "Flat 1 Hz input tone case only; no spectrum-density integration or full RFAMP equivalence",
        "source_power_dbm": source_power_dbm, "run_started_utc": capture["run_started_utc"],
        "parameters": verified["parameters"], "relative_tolerance": 1e-7,
        "checks": checks, "passed": all(check["passed"] for check in checks),
        "effective_drive_diagnostic": {
            "scope": "Power ratios inferred independently from measured H2 and H3; not a fitted predictor",
            "from_h2": ratios[0], "from_h3": ratios[1],
            "relative_disagreement": ratios[1] / ratios[0] - 1,
            "affects_compatibility_verdict": False,
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source-power-dbm", type=float, required=True)
    parser.add_argument("--output-saturation-dbm", type=int, choices=(22, 23, 26))
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.capture.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.capture.read_bytes()
    report = compare(single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")),
                     args.source_power_dbm, output_saturation_dbm=args.output_saturation_dbm)
    report["capture_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
