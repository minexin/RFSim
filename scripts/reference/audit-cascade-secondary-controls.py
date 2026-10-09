"""Audit secondary-spectrum controls without claiming RFAMP high-order compatibility."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "cascade", Path(__file__).with_name("compare-cascade-amplifier.py")
)
cascade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cascade)


def audit(captures):
    expected = {
        (3, False, -140),
        (3, True, -140),
        (3, True, 50),
        (3, True, 140),
        (5, False, -140),
        (5, True, -140),
    }
    indexed = {}
    for capture in captures:
        cascade.inspect(capture)
        if (
            capture["source_power_dbm"] != -10
            or capture["source_phase_deg"] != 90
            or capture["two_tone"] is not True
            or capture["second_gain_db"] != 10
            or capture["reverse_isolation_db"] != 140
            or capture["channel_bandwidth_hz"] != 1e6
        ):
            raise ValueError("Uncontrolled secondary-spectrum experiment")
        key = (
            capture["maximum_order"],
            capture["secondary_spectrum"],
            capture["secondary_range_db"],
        )
        if key in indexed:
            raise ValueError("Duplicate control configuration")
        indexed[key] = capture
    if set(indexed) != expected:
        raise ValueError("Incomplete secondary-spectrum controls")
    comparisons = []
    for order, enabled, threshold in sorted(expected):
        reference = indexed[(order, False, -140)]
        capture = indexed[(order, enabled, threshold)]
        for port in (2, 3):
            left, right = cascade.spectrum(reference, port), cascade.spectrum(capture, port)
            if left.keys() != right.keys():
                raise ValueError("Secondary controls changed identities; investigate new evidence")
            errors = []
            for identity, rows in left.items():
                other = right[identity]
                if len(rows) != len(other):
                    raise ValueError("Control spectrum dimensions changed")
                for a, b in zip(rows, other):
                    if a[0] != b[0] or a[2] != b[2]:
                        raise ValueError("Control frequency/coherence changed")
                    errors.append(abs(a[1] - b[1]) / abs(a[1]))
            comparisons.append(
                {
                    "maximum_order": order,
                    "secondary_spectrum": enabled,
                    "secondary_range_db": threshold,
                    "port": port,
                    "rf_origins": len(left),
                    "raw_capture_sha256": capture["raw_capture_sha256"],
                    "max_relative_wave_change": max(errors),
                }
            )
    return {
        "classification": "diagnostic_only",
        "eligible_for_compatibility": False,
        "observations": comparisons,
        "controls_unchanged": all(item["max_relative_wave_change"] == 0 for item in comparisons),
        "conclusion": "No secondary-switch effect observed in these controlled cascades",
        "limitation": "Five-order native paths do not establish their coefficients, recursive order rules or sub-spectrum merging",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() == args.captures.resolve():
        parser.error("Output must not overwrite captures")
    raw = args.captures.read_bytes()
    report = audit(json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["controls_unchanged"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
