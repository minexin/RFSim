import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-antenna-compression.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class NativeCompressionComparisonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.Library(library_path)
        validation = comparison.ROOT / "validation"
        cls.sweep = json.loads((validation / "systemvue-2023-antenna-power-sweep.json").read_text())
        cls.settings = json.loads((validation / "systemvue-2023-antenna-nonlinear-settings.json").read_text())

    def test_archived_signal_comparison(self):
        report = comparison.compare(self.library, self.sweep, self.settings)
        self.assertEqual(len(report["checks"]), 40)
        self.assertTrue(report["passed"])
        self.assertEqual(report["relative_tolerance"], 1e-7)

    def test_changed_measurement_fails_without_fitting(self):
        sweep = copy.deepcopy(self.sweep)
        sweep["points"][-1]["nodes"][-1]["gain"] *= 1.001
        report = comparison.compare(self.library, sweep, self.settings)
        failures = [check for check in report["checks"] if not check["passed"]]
        self.assertFalse(report["passed"])
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["metric"], "gain")

    def test_near_compression_gap_remains_visible(self):
        validation = comparison.ROOT / "validation"
        sweep = json.loads((validation / "systemvue-2023-antenna-near-compression-sweep.json").read_text())
        settings = json.loads((validation / "systemvue-2023-antenna-near-compression-settings.json").read_text())
        report = comparison.compare(self.library, sweep, settings)
        self.assertFalse(report["passed"])
        self.assertEqual(len(report["checks"]), 20)
        failures = [check for check in report["checks"] if not check["passed"]]
        self.assertEqual(len(failures), 4)
        self.assertEqual({check["node"] for check in failures}, {"RFAmp2"})
        # Guard against silently weakening the comparator to conceal the observed gap.
        self.assertEqual(report["relative_tolerance"], 1e-7)
        self.assertTrue(all(check["signed_relative_error"] > 1e-6 for check in failures))

    def test_rejects_inconsistent_evidence(self):
        for modification in ("hash", "power", "order", "empty"):
            sweep = copy.deepcopy(self.sweep)
            if modification == "hash":
                sweep["points"][0]["capture_sha256"] = "unrelated"
            elif modification == "power":
                sweep["points"][0]["source_power_w"] *= 2
            elif modification == "order":
                sweep["points"][0]["nodes"].reverse()
            else:
                sweep["points"] = []
            with self.subTest(modification=modification), self.assertRaises(ValueError):
                comparison.compare(self.library, sweep, self.settings)


if __name__ == "__main__":
    unittest.main()
