"""Replay the unified native amplifier against independently archived source spectra."""

import importlib.util
import json
from pathlib import Path
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "highorder_input", Path(__file__).with_name("diagnose-highorder-input-reference.py")
)
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class HighOrderModelReferenceTests(unittest.TestCase):
    def test_unified_native_model_matches_measured_input_holdouts(self):
        library = reference.comparison.traced.two_tone.single.Library(LIBRARY)
        captures = json.loads(
            (ROOT / "validation/systemvue-2023-eleventh-order-captures.json").read_text()
        )
        inputs = {
            r["raw_capture_sha256"]: r
            for r in json.loads(
                (ROOT / "validation/systemvue-2023-highorder-input-references.json").read_text()
            )
        }
        evidence = json.loads(
            (ROOT / "validation/systemvue-2023-highorder-input-diagnostic.json").read_text()
        )
        archived_cases = {c["raw_capture_sha256"]: c for c in evidence["cases"]}
        nonlinear = [0.0] * 10
        for order, value in evidence["even_rules"].items():
            nonlinear[int(order) - 2] = value["voltage_coefficient"]
        for order, value in evidence["odd_hypotheses"].items():
            nonlinear[int(order) - 2] = value["effective_voltage_coefficient"]
        checked = 0
        for capture in captures:
            digest = capture["raw_capture_sha256"]
            sources, observations = reference.measured_sources(capture, inputs[digest])
            result = library.highorder_amplifier(
                1e8,
                sources,
                nonlinear[: capture["maximum_order"] - 1],
                power_gain_db=10,
                output_p1db_dbm=20,
                output_saturation_dbm=23,
            )
            self.assertAlmostEqual(
                result.total_input_power_w / observations["fixed_reference_drive_w"], 1, places=14
            )
            observed = reference.comparison.spectrum(capture, 3)
            missing = set()
            for term in result.terms:
                if term.order == 1:
                    continue
                label = reference.base.term_label(term, result.inputs)
                if label not in observed:
                    missing.add(label)
                    continue
                samples = reference.base.helpers.pair(
                    observed[label], term.component.bin * 1e8, term.component.bandwidth_hz
                )
                if (
                    reference.base.signature(capture) == reference.base.CALIBRATION
                    and term.order in reference.base.ODD_ORDERS
                ):
                    continue
                error = max(abs(sample[1] / term.component.amplitude - 1) for sample in samples)
                self.assertLessEqual(error, 1e-7)
                checked += 1
            self.assertEqual(missing, {c["label"] for c in archived_cases[digest]["not_recorded"]})
        self.assertEqual(checked, 1413)


if __name__ == "__main__":
    unittest.main()
