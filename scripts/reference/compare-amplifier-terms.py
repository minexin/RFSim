"""Compare individual local mixing paths, including carrier-overlapping cubic terms."""
import argparse
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


def compare(library, captures):
    if not isinstance(captures, list) or not captures:
        raise ValueError("Nonempty controlled sweep required")
    reports, seen = [], set()
    for capture in captures:
        powers_dbm = source_powers(capture)
        if powers_dbm in seen:
            raise ValueError("Duplicate ordered power pair")
        seen.add(powers_dbm)
        power, parameters = two_tone.inspect(capture, powers_dbm[0], second_power_dbm=powers_dbm[1])
        second_power = 10 ** ((powers_dbm[1] - 30) / 10)
        observed = two_tone.spectrum_observer(capture)
        traced = library.multitone_amplifier_terms(1e8, {10: math.sqrt(power), 11: math.sqrt(second_power)},
            power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=23, input_ip2_dbm=20, input_ip3_dbm=10)
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
        reports.append({"source_powers_dbm": list(powers_dbm), "parameters": parameters,
                        "raw_capture_sha256": capture["raw_capture_sha256"],
                        "run_started_utc": capture["run_started_utc"],
                        "manager_messages": capture["manager_errors"], "checks": checks})
    return {"scope": "Sixteen separately identified direct/second/third-order RF terms per controlled two-tone case",
            "limitation": "Individual powers only; no measured phases, coherent carrier sums or multi-stage provenance",
            "relative_tolerance": 1e-7, "reports": reports,
            "passed": all(c["passed"] for r in reports for c in r["checks"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(two_tone.single.Library(args.library.resolve()), json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
