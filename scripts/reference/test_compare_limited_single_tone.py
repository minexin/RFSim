import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-limited-single-tone.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class LimitedToneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.harmonics.single.Library(library_path)
        cls.captures = json.loads((comparison.harmonics.single.ROOT / "validation" /
            "systemvue-2023-limited-single-tone-captures.json").read_text())

    def test_all_nine_cases_with_original_gaps_preserved(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["reports"]), 9)
        self.assertEqual(sum(len(r["checks"]) for r in report["reports"]), 27)
        validation = [r for r in report["reports"] if r["independent_parameter_cross_validation"]]
        self.assertEqual({r["source_power_dbm"] for r in validation}, {3, 5})
        self.assertTrue(any(not c["passed"] for r in report["reports"] for c in r["unlimited_harmonic_checks"]))
        self.assertTrue(all(not r["manager_messages"] for r in report["reports"]))

    def test_profile_must_be_explicit_and_parameters_checked(self):
        capture = copy.deepcopy(self.captures[-1])
        capture["output_saturation_dbm"] = 23
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])
        capture["output_saturation_dbm"] = 18
        with self.assertRaises(ValueError):
            comparison.harmonics.compare(self.library, capture, capture["source_power_dbm"])
        next(n for n in capture["nodes"] if n["path"].endswith("RFAmp/ParamSet/OP1dB"))["data"] = .1
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])

    def test_changed_output_is_not_refitted_and_warnings_reject(self):
        capture = copy.deepcopy(self.captures[-1])
        next(n for n in capture["nodes"] if n["path"].endswith("System1_Data_Path1/Eqns/VarBlock/DCP"))["data"][1] *= 1.01
        self.assertFalse(comparison.compare(self.library, [capture])["passed"])
        capture["manager_errors"] = "(WARNING) diagnostic only"
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures + [self.captures[0]])


if __name__ == "__main__":
    unittest.main()
