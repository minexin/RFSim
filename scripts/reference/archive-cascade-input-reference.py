"""Preserve only measured source-input rows, linked to existing raw capture hashes."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "input_reference_archive", Path(__file__).with_name("archive-cascade-reference.py")
)
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)
comparison = archive.comparison


def extract(raw, status, digest):
    verified = archive.extract(raw, status, digest)
    comparison.spectrum(raw, 1)
    names = dict(zip(comparison.vector(raw, "IDNo"), comparison.vector(raw, "IDName")))
    identifiers = comparison.vector(raw, "ID1")
    frequencies = comparison.vector(raw, "F1")
    powers = comparison.vector(raw, "P1")
    voltages = comparison.vector(raw, "V1")
    impedances = comparison.vector(raw, "Z1")
    sources = []
    for index in range(1, 3 if verified["two_tone"] else 2):
        label = f"D[(Source.Source{index})],Source"
        selected = []
        for row, identifier in enumerate(identifiers):
            # The existing spectrum parser validates the complete ID mapping.
            name = names[identifier]
            if name.startswith("{") and name.split("}", 1)[1] == label:
                selected.append(
                    {
                        "frequency_hz": frequencies[row],
                        "power_w": powers[row],
                        "peak_voltage": voltages[2 * row : 2 * row + 2],
                        "impedance_ohms": impedances[2 * row : 2 * row + 2],
                    }
                )
        if len(selected) != 2:
            raise ValueError("Expected exactly two measured source-input band edges")
        sources.append({"source_index": index, "label": label, "samples": selected})
    return {
        "raw_capture_sha256": digest,
        "reference_ohms": comparison.value(verified, "Design3/PartList/RFAmp1/ParamSet/Zref"),
        "source_port": 1,
        "sources": sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    protected = {
        p.resolve() for run in args.runs for p in (run / "capture.json", run / "status.json")
    }
    if args.output.resolve() in protected:
        parser.error("Output must not overwrite capture inputs")
    records = []
    for run in args.runs:
        data = (run / "capture.json").read_bytes()
        records.append(
            extract(
                json.loads(data.decode("utf-8-sig")),
                json.loads((run / "status.json").read_text()),
                hashlib.sha256(data).hexdigest(),
            )
        )
    if len({r["raw_capture_sha256"] for r in records}) != len(records):
        raise ValueError("Duplicate raw capture")
    args.output.write_text(json.dumps(records, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
