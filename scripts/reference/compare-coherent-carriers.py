"""Compare explicit coherence reduction with two controlled node carrier totals."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location("terms", Path(__file__).with_name("compare-amplifier-terms.py"))
terms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terms)
from rfmodel import CoherentComponent, SpectrumKind


def compare(library, captures):
    individual = terms.compare(library, captures, complex_amplitudes=True)
    reports = []
    for capture, reference in zip(captures, individual["reports"]):
        entries = [c for c in reference["checks"] if c["bin"] in (10, 11)]
        components = [
            CoherentComponent(c["bin"], SpectrumKind.SOURCE if c["order"] == 1 else SpectrumKind.INTERMOD,
                              c["frequency_bounds_hz"][1] - c["frequency_bounds_hz"][0],
                              c["coherency_number"], complex(*c["rfmodel_amplitude"]))
            for c in entries]
        result = library.reduce_coherent_components(1e8, components)
        if len(entries) != 6 or len(result.components) != 6:
            raise ValueError("Expected three distinct coherent groups per carrier")
        vector = lambda name: terms.two_tone.vector(capture, name)
        names = dict(zip(vector("IDNo"), vector("IDName")))
        points = list(zip(vector("F2"), vector("P2"), vector("ID2")))
        noise = [p for _, p, identifier in points if names[identifier] == "Node Noise from 'RFAmp'"]
        if not noise or any(not math.isfinite(p) or p < 0 for p in noise):
            raise ValueError("Expected finite exported output-noise samples")
        checks = []
        for index in (10, 11):
            measured = [p for f, p, identifier in points
                        if f == index * 1e8 and names[identifier] == "Node Total from 'RFAmp'"]
            if len(measured) != 1 or not math.isfinite(measured[0]) or measured[0] <= 0:
                raise ValueError("Missing/ambiguous carrier-center total sample")
            observed = measured[0]
            # Do not hide a noise-dominated comparison behind signal-only prediction.
            noise_bound = max(noise) / observed
            if noise_bound > 1e-8:
                raise ValueError("Exported noise is too large for this signal-only carrier comparison")
            error = result.power_by_bin_w[index] / observed - 1
            checks.append({"bin": index, "systemvue_total_w": observed,
                           "rfmodel_signal_w": result.power_by_bin_w[index],
                           "exported_noise_fraction_bound": noise_bound,
                           "signed_relative_error": error, "passed": abs(error) <= 1e-7})
        reports.append({"profile": reference["profile"],
                        "source_powers_dbm": reference["source_powers_dbm"],
                        "source_phases_deg": reference["source_phases_deg"],
                        "raw_capture_sha256": capture["raw_capture_sha256"], "checks": checks})
    return {"scope": "Two carrier-center powers with three explicit independent groups each",
            "coherency_assignment": "Imported from SystemVue; not inferred by RFModel",
            "limitation": "No automatic clocks, coherent path merging, noise prediction or band integration",
            "relative_tolerance": 1e-7, "individual_terms_passed": individual["passed"],
            "reports": reports,
            "passed": individual["passed"] and all(c["passed"] for r in reports for c in r["checks"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (args.library.resolve(), args.captures.resolve()):
        parser.error("Output must not overwrite inputs")
    raw = args.captures.read_bytes()
    report = compare(terms.two_tone.single.Library(args.library.resolve()),
                     json.loads(raw.decode("utf-8-sig")))
    report["captures_sha256"] = hashlib.sha256(raw).hexdigest()
    report["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
