"""Protect odd-order holdouts and preserve observed even-order failures."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "eleventh_diagnostic", Path(__file__).with_name("diagnose-eleventh-order.py")
)
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)
comparison = diagnostic.comparison


class EleventhOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.traced.two_tone.single.Library(LIBRARY)
        cls.captures = json.loads(
            (ROOT / "validation/systemvue-2023-eleventh-order-captures.json").read_text()
        )

    def report(self, captures=None):
        return diagnostic.diagnose(self.library, self.captures if captures is None else captures)

    def calibration(self, captures):
        return next(c for c in captures if diagnostic.signature(c) == diagnostic.CALIBRATION)

    def harmonic_indices(self, capture, order):
        label = comparison.label(comparison.traced.expression((10,) * order), 1)
        number = next(
            n
            for n, name in zip(
                comparison.vector(capture, "IDNo"), comparison.vector(capture, "IDName")
            )
            if name.startswith("{") and name.endswith(label)
        )
        return [i for i, n in enumerate(comparison.vector(capture, "ID3")) if n == number]

    def change_harmonic(self, capture, order, factor):
        indices = self.harmonic_indices(capture, order)
        self.assertEqual(len(indices), 2)
        voltages = comparison.vector(capture, "V3")
        powers = comparison.vector(capture, "P3")
        for i in indices:
            value = complex(voltages[2 * i], voltages[2 * i + 1]) * factor
            voltages[2 * i : 2 * i + 2] = [value.real, value.imag]
            powers[i] *= abs(factor) ** 2

    def test_actual_results_keep_even_failures_and_odd_holdouts(self):
        report = self.report()
        self.assertEqual(report["classification"], "diagnostic_only")
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertEqual(report["relative_wave_tolerance"], 1e-7)
        self.assertEqual(
            [s["consistent"] for s in report["even_rules"].values()], [True, True, False, False]
        )
        for order, count in ((5, 68), (7, 136), (9, 235), (11, 368)):
            result = report["odd_hypotheses"][str(order)]
            self.assertEqual(result["observed_count"], count)
            self.assertTrue(result["consistent"])
            self.assertLess(result["max_relative_wave_error"], 9e-9)
        self.assertEqual(report["even_rules"]["6"]["reference_product"], [4, -2])
        self.assertEqual(report["even_rules"]["10"]["reference_product"], [6, -4])
        self.assertGreater(report["even_rules"]["10"]["max_relative_wave_error"], 1e-7)

    def test_common_input_limiting_explains_high_power_two_tone(self):
        case = next(
            c
            for c in self.report()["cases"]
            if c["configuration"]["two_tone"] and c["configuration"]["second_source_power_dbm"] == 5
        )
        self.assertAlmostEqual(
            case["operating_point"]["nonlinear_input_scale"], 0.9954103717335859, places=14
        )
        checks = [c for c in case["checks"] if c["order"] == 11]
        self.assertEqual(len(checks), 182)
        self.assertGreater(max(c["unscaled_relative_wave_error"] for c in checks), 0.049)
        self.assertTrue(all(c["passed"] for c in checks))

    def test_unrecorded_terms_remain_explicit(self):
        report = self.report()
        self.assertEqual(sum(len(c["not_recorded"]) for c in report["cases"]), 10)
        for case in report["cases"]:
            if case["not_recorded"]:
                self.assertFalse(case["all_predicted_origins_recorded"])
                observed = {c["label"] for c in case["checks"]}
                self.assertTrue(all(c["label"] not in observed for c in case["not_recorded"]))

    def test_incomplete_duplicate_and_uncontrolled_experiments_fail(self):
        for captures in ([], self.captures[:-1], self.captures + self.captures[:1]):
            with self.assertRaises(ValueError):
                self.report(captures)
        for key, value in (
            ("spectrum_reduction", True),
            ("second_gain_db", 10),
            ("maximum_order", 12),
            ("source_power_dbm", 6),
        ):
            captures = copy.deepcopy(self.captures)
            captures[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.report(captures)

    def test_calibration_change_cannot_recalibrate_holdouts(self):
        captures = copy.deepcopy(self.captures)
        self.change_harmonic(self.calibration(captures), 11, 1.01)
        report = self.report(captures)
        self.assertFalse(report["odd_hypotheses"]["11"]["consistent"])
        self.assertGreater(report["odd_hypotheses"]["11"]["max_relative_wave_error"], 0.009)
        self.assertTrue(report["odd_hypotheses"]["9"]["consistent"])

    def test_complex_calibration_is_rejected(self):
        captures = copy.deepcopy(self.captures)
        self.change_harmonic(self.calibration(captures), 11, 1j)
        with self.assertRaises(ValueError):
            self.report(captures)

    def test_missing_required_high_order_origin_is_rejected(self):
        captures = copy.deepcopy(self.captures)
        capture = self.calibration(captures)
        removed = set(self.harmonic_indices(capture, 11))
        for name in ("F", "P", "ID", "V", "Z"):
            node = comparison.node(capture, comparison.SPECTRUM + name + "3")
            width = 2 if name in ("V", "Z") else 1
            node["data"] = [
                value for i, value in enumerate(node["data"]) if i // width not in removed
            ]
            node["dimensions"] = [len(node["data"])]
        with self.assertRaisesRegex(ValueError, "Missing isolated harmonic"):
            self.report(captures)

    def test_truncated_complex_array_is_rejected(self):
        captures = copy.deepcopy(self.captures)
        comparison.node(captures[0], comparison.SPECTRUM + "V3")["data"] = None
        with self.assertRaisesRegex(ValueError, "Malformed spectrum array"):
            self.report(captures)


if __name__ == "__main__":
    unittest.main()
