import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-saturating-fundamental.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class NativeSaturationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.single.Library(library_path)
        cls.captures = json.loads((comparison.single.ROOT / "validation" /
            "systemvue-2023-incremental-tanh-captures.json").read_text())

    def test_native_response_and_new_cross_validation_points(self):
        report = comparison.compare(self.library, self.captures)
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertFalse(report["coefficients_fitted_to_capture"])
        self.assertTrue(report["all_within_diagnostic_tolerance"])
        self.assertEqual({(r["source_power_dbm"], r["output_saturation_dbm"]) for r in report["reports"]},
                         {(-30, 23), (.9, 23), (2, 23), (3, 23), (6, 23), (10, 23), (8, 26)})
        self.assertNotIn("passed", report)

    def test_output_change_exposes_gap_instead_of_fitting_it(self):
        capture = copy.deepcopy(self.captures[-1])
        node = next(n for n in capture["nodes"]
                    if n["path"].endswith("System1_Data_Path1/Eqns/VarBlock/DCP"))
        node["data"][1] *= 1.01
        report = comparison.compare(self.library, [capture])
        self.assertFalse(report["all_within_diagnostic_tolerance"])

    def test_default_strict_validation_still_rejects_warning(self):
        capture = next(c for c in self.captures if c["source_power_dbm"] == 3)
        with self.assertRaises(ValueError):
            comparison.single.runner.validate_capture(capture, "compression")
        changed = copy.deepcopy(capture)
        changed["manager_errors"] += "\n(ERROR) invalid state"
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [changed])
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture, capture])


if __name__ == "__main__":
    unittest.main()
