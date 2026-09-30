import copy
import importlib.util
import json
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "diagnostic", Path(__file__).with_name("diagnose-single-saturation.py"))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class SaturationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.captures = json.loads((diagnostic.single.ROOT / "validation" /
            "systemvue-2023-single-saturation-diagnostic-captures.json").read_text())

    def test_formula_anchors_continuity_and_saturation(self):
        predict = diagnostic.continuous_tanh_power
        gain, output_p1, saturation = 100., .1, 10 ** (-.7)
        input_p1 = output_p1 / gain * 10 ** .1
        self.assertEqual(predict(0, gain, output_p1, saturation), 0)
        self.assertAlmostEqual(predict(1e-15, gain, output_p1, saturation) / 1e-15, gain, places=8)
        self.assertAlmostEqual(predict(input_p1, gain, output_p1, saturation), output_p1, places=14)
        x1 = math.sqrt(input_p1)
        step = x1 * 1e-6
        amplitude = lambda x: math.sqrt(predict(x * x, gain, output_p1, saturation))
        left = (amplitude(x1) - amplitude(x1 - step)) / step
        right = (amplitude(x1 + step) - amplitude(x1)) / step
        expected = math.sqrt(gain) * (3 * 10 ** (-.05) - 2)
        self.assertLess(abs(left / expected - 1), 1e-6)
        self.assertLess(abs(right / expected - 1), 1e-6)
        values = [predict(input_p1 * scale, gain, output_p1, saturation) for scale in (1, 2, 5, 20, 100)]
        self.assertTrue(all(a < b for a, b in zip(values, values[1:])))
        self.assertLessEqual(values[-1], saturation)
        self.assertAlmostEqual(values[-1] / saturation, 1., places=7)
        for args in ((-1, gain, output_p1, saturation), (1, 0, output_p1, saturation),
                     (1, gain, output_p1, output_p1), (math.nan, gain, output_p1, saturation)):
            with self.assertRaises(ValueError):
                predict(*args)

    def test_observed_gap_is_diagnostic_only(self):
        report = diagnostic.diagnose(self.captures)
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertFalse(report["coefficients_fitted_to_capture"])
        self.assertNotIn("passed", report)
        self.assertEqual({r["source_power_dbm"] for r in report["reports"]}, {3, 6, 10})
        for item in report["reports"]:
            self.assertIn("less accuracy", item["manager_messages"])
            self.assertLess(item["signed_relative_error"], -.01)
            self.assertNotIn("passed", item)
        with self.assertRaises(ValueError):
            diagnostic.diagnose(self.captures + [self.captures[0]])

    def test_default_validation_and_unexpected_messages_reject(self):
        for original in self.captures:
            with self.assertRaises(ValueError):
                diagnostic.single.runner.validate_capture(original, "compression")
            for message in ("(ERROR) failed", "(INFO) parameter outside range",
                            original["manager_errors"] + "\n(ERROR) another problem",
                            original["manager_errors"].replace("RFAmp", "OtherAmp")):
                capture = copy.deepcopy(original)
                capture["manager_errors"] = message
                with self.assertRaises(ValueError):
                    diagnostic.diagnose([capture])

    def test_warning_mode_still_checks_freshness_parameters_and_topology(self):
        for kind in ("stale", "parameter", "topology"):
            capture = copy.deepcopy(self.captures[0])
            if kind == "stale":
                next(n for n in capture["nodes"] if n["path"].endswith("/System1_Data_Path1"))["timestamp"] = "1"
            elif kind == "parameter":
                next(n for n in capture["nodes"] if n["path"].endswith("RFAmp/ParamSet/G"))["data"] = 10
            else:
                capture["nodes"].append({"path": diagnostic.single.BASE + "Sch1/PartList/OtherAmp"})
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                diagnostic.diagnose([capture])


if __name__ == "__main__":
    unittest.main()
