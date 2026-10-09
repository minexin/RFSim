"""Validate high-order diagnostics without promoting fitted data to compatibility."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "highorder", Path(__file__).with_name("diagnose-highorder-amplifier.py")
)
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class HighOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = diagnostic.comparison.traced.two_tone.single.Library(LIBRARY)
        cls.captures = json.loads(
            (ROOT / "validation/systemvue-2023-highorder-captures.json").read_text()
        )

    def report(self, captures=None):
        return diagnostic.diagnose(self.library, self.captures if captures is None else captures)

    def change_wave(self, captures, order, factor):
        capture = next(c for c in captures if diagnostic.signature(c) == diagnostic.CALIBRATION)
        comparison = diagnostic.comparison
        names = comparison.vector(capture, "IDName")
        numbers = comparison.vector(capture, "IDNo")
        label = comparison.label(comparison.traced.expression((10,) * order), 1)
        number = next(
            number
            for number, name in zip(numbers, names)
            if name.endswith(label) and name.startswith("{")
        )
        ids = comparison.vector(capture, "ID3")
        volts = comparison.vector(capture, "V3")
        power = comparison.vector(capture, "P3")
        for index, identifier in enumerate(ids):
            if identifier == number:
                wave = complex(volts[2 * index], volts[2 * index + 1]) * factor
                volts[2 * index], volts[2 * index + 1] = wave.real, wave.imag
                power[index] *= abs(factor) ** 2

    def test_actual_cases_preserve_scope_and_missing_origins(self):
        report = self.report()
        self.assertEqual(report["classification"], "diagnostic_only")
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertFalse(report["all_predicted_origins_recorded"])
        self.assertEqual(report["fourth_order_rule"]["observed_count"], 23)
        self.assertTrue(report["fourth_order_rule"]["consistent"])
        self.assertLess(report["fourth_order_rule"]["max_relative_wave_error"], 1e-7)
        fifth = report["fifth_order_hypothesis"]
        self.assertEqual(fifth["observed_count"], 27)
        self.assertTrue(fifth["consistent"])
        self.assertAlmostEqual(fifth["fitted_voltage_coefficient"], -0.07786106164494842, places=13)
        gaps = report["two_tone_missing_origins"]
        self.assertEqual(sum(g["order"] == 4 for g in gaps), 2)
        self.assertEqual(sum(g["order"] == 5 for g in gaps), 10)

    def test_missing_low_power_observations_are_not_zero_or_passes(self):
        report = self.report()
        low = next(c for c in report["cases"] if c["configuration"]["source_power_dbm"] == -30)
        self.assertEqual(low["fifth_order"]["observed_count"], 0)
        self.assertIsNone(low["fifth_order"]["consistent"])
        self.assertIsNone(low["fifth_order"]["max_relative_wave_error"])
        self.assertEqual(len(low["not_recorded"]), 4)

    def test_incomplete_duplicate_and_uncontrolled_sets_are_rejected(self):
        for captures in ([], self.captures[:-1], self.captures + self.captures[:1]):
            with self.subTest(count=len(captures)), self.assertRaises(ValueError):
                self.report(captures)
        captures = copy.deepcopy(self.captures)
        captures[0]["maximum_order"] = 3
        with self.assertRaises(ValueError):
            self.report(captures)

    def test_fourth_phase_error_is_not_hidden_by_fifth_fitting(self):
        captures = copy.deepcopy(self.captures)
        self.change_wave(captures, 4, 1j)
        report = self.report(captures)
        self.assertFalse(report["fourth_order_rule"]["consistent"])
        self.assertTrue(report["fifth_order_hypothesis"]["consistent"])
        self.assertFalse(report["eligible_for_compatibility"])

    def test_fifth_calibration_error_fails_independent_holdouts(self):
        captures = copy.deepcopy(self.captures)
        self.change_wave(captures, 5, 1.1)
        report = self.report(captures)
        self.assertFalse(report["fifth_order_hypothesis"]["consistent"])
        self.assertTrue(report["fourth_order_rule"]["consistent"])

    def test_complex_fifth_coefficient_is_rejected(self):
        captures = copy.deepcopy(self.captures)
        self.change_wave(captures, 5, 1j)
        with self.assertRaisesRegex(ValueError, "real voltage coefficient"):
            self.report(captures)

    def test_corrupt_bandwidth_is_rejected(self):
        captures = copy.deepcopy(self.captures)
        capture = next(c for c in captures if diagnostic.signature(c) == diagnostic.CALIBRATION)
        fs = diagnostic.comparison.vector(capture, "F3")
        for i, f in enumerate(fs):
            if abs(f - 5e9) < 10:
                fs[i] += 0.1
        with self.assertRaisesRegex(ValueError, "frequency/bandwidth"):
            self.report(captures)


if __name__ == "__main__":
    unittest.main()
