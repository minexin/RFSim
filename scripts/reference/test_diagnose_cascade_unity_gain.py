"""Keep unity-gain diagnosis separate from nominal compatibility acceptance."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "unity_gain", Path(__file__).with_name("diagnose-cascade-unity-gain.py")
)
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class UnityGainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = diagnostic.comparison.traced.two_tone.single.Library(LIBRARY)
        cls.captures = json.loads(
            (ROOT / "validation/systemvue-2023-cascade-gain-captures.json").read_text()
        )

    def test_actual_sweep_and_independent_holdouts(self):
        report = diagnostic.diagnose(self.library, self.captures)
        self.assertEqual(report["classification"], "diagnostic_only")
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertFalse(report["nominal_passed"])
        self.assertTrue(report["hypothesis_consistent"])
        self.assertEqual(len(report["cases"]), 17)
        self.assertEqual(sum(not case["nominal"]["passed"] for case in report["cases"]), 5)
        self.assertAlmostEqual(report["inferred_linear_power_gain"], 1.00000023, places=13)
        self.assertLess(report["explicit_equivalent_max_relative_wave_difference"], 1e-12)
        for case in report["cases"]:
            self.assertTrue(case["gain_hypothesis"]["passed"])
            self.assertEqual(
                case["nominal"]["passed"], case["configuration"]["second_gain_db"] != 0
            )

    def test_missing_sweep_or_holdout_and_duplicates_are_rejected(self):
        for cases in (
            [],
            self.captures[:-1],
            [case for case in self.captures if not case["two_tone"]],
            self.captures + self.captures[:1],
        ):
            with self.subTest(count=len(cases)), self.assertRaises(ValueError):
                diagnostic.diagnose(self.library, cases)

    def test_uncontrolled_settings_are_rejected(self):
        cases = copy.deepcopy(self.captures)
        cases[0]["reverse_isolation_db"] = 100
        with self.assertRaises(ValueError):
            diagnostic.diagnose(self.library, cases)

    def test_phase_corruption_cannot_be_explained_by_scalar_gain(self):
        cases = copy.deepcopy(self.captures)
        zero = next(
            case
            for case in cases
            if case["second_gain_db"] == 0 and case["source_power_dbm"] == -20
        )
        voltages = diagnostic.comparison.node(zero, diagnostic.comparison.SPECTRUM + "V2")
        voltages["data"] = [-value for value in voltages["data"]]
        report = diagnostic.diagnose(self.library, cases)
        self.assertFalse(report["hypothesis_consistent"])
        self.assertFalse(report["explicit_equivalent_matches"])

    def test_equivalent_capture_is_an_independent_check(self):
        cases = copy.deepcopy(self.captures)
        equivalent = next(case for case in cases if 9e-7 < case["second_gain_db"] < 1e-6)
        for name, factor in (("V2", 1.00001), ("P2", 1.00001**2)):
            values = diagnostic.comparison.node(equivalent, diagnostic.comparison.SPECTRUM + name)
            values["data"] = [value * factor for value in values["data"]]
        report = diagnostic.diagnose(self.library, cases)
        self.assertTrue(report["estimates_agree"])
        self.assertFalse(report["explicit_equivalent_matches"])
        self.assertFalse(report["hypothesis_consistent"])

    def test_nominal_library_is_unchanged_after_diagnosis(self):
        before = diagnostic.comparison.compare(self.library, self.captures)
        diagnostic.diagnose(self.library, self.captures)
        after = diagnostic.comparison.compare(self.library, self.captures)
        self.assertEqual(before, after)
        self.assertFalse(after["passed"])


if __name__ == "__main__":
    unittest.main()
