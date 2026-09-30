import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-single-compression.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class SingleCompressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.Library(library_path)
        cls.capture = json.loads((comparison.ROOT / "validation" /
            "systemvue-2023-single-compression-plus09-capture.json").read_text())

    def test_fresh_controlled_reference(self):
        report = comparison.compare(self.library, self.capture, .9)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["checks"]), 2)
        self.assertEqual(len(report["parameters"]), 16)

    def test_single_source_enum_scalar_and_array_forms(self):
        capture = copy.deepcopy(self.capture)
        for name in ("Enable", "SrcType", "EnablePN"):
            entry = next(n for n in capture["nodes"] if n["path"].endswith("Source/ParamSet/" + name))
            entry["data"] = entry["data"][0]
            entry["dimensions"] = None
        self.assertTrue(comparison.compare(self.library, capture, .9)["passed"])
        entry = next(n for n in capture["nodes"] if n["path"].endswith("Source/ParamSet/Enable"))
        for invalid in ([1, 1], True, [True], 0, [0], "1", float("nan")):
            entry["data"] = invalid
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                comparison.compare(self.library, capture, .9)

    def test_rejects_warning_changed_parameter_and_stale_data(self):
        for kind in ("warning", "parameter", "timestamp", "topology"):
            capture = copy.deepcopy(self.capture)
            if kind == "warning":
                capture["manager_errors"] = "(WARNING) reduced accuracy"
            elif kind == "parameter":
                next(n for n in capture["nodes"] if n["path"].endswith("RFAmp/ParamSet/G"))["data"] = 10
            elif kind == "timestamp":
                next(n for n in capture["nodes"] if n["path"].endswith("/System1_Data_Path1"))["timestamp"] = "1"
            else:
                capture["nodes"].append({"path": comparison.BASE + "Sch1/PartList/Unexpected"})
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                comparison.compare(self.library, capture, .9)

    def test_reverse_isolation_control_and_required_declaration(self):
        capture = json.loads((comparison.ROOT / "validation" /
            "systemvue-2023-single-compression-plus09-riso50-capture.json").read_text())
        with self.assertRaises(ValueError):
            comparison.compare(self.library, capture, .9)
        report = comparison.compare(self.library, capture, .9, reverse_isolation_db=50)
        baseline = comparison.compare(self.library, self.capture, .9)
        self.assertTrue(report["passed"])
        for metric in ("CGAIN", "DCP"):
            relative_change = report["measurements"][metric][1] / baseline["measurements"][metric][1] - 1
            self.assertLess(abs(relative_change), 1e-9)
        changed = [a["parameter"] for a, b in zip(baseline["parameters"], report["parameters"])
                   if a["value"] != b["value"]]
        self.assertEqual(changed, ["RFAmp/RISO"])

    def test_antenna_parameter_profile_requires_explicit_selection(self):
        capture = json.loads((comparison.ROOT / "validation" /
            "systemvue-2023-single-compression-antenna-plus30-capture.json").read_text())
        with self.assertRaises(ValueError):
            comparison.compare(self.library, capture, 30, reverse_isolation_db=50)
        report = comparison.compare(self.library, capture, 30,
                                    reverse_isolation_db=50, profile="antenna")
        self.assertTrue(report["passed"])
        self.assertEqual(report["measurements"]["CF"], [5e9, 5e9])
        self.assertEqual(report["profile"], "antenna")
        changed = copy.deepcopy(capture)
        next(n for n in changed["nodes"] if n["path"].endswith("RFAmp/ParamSet/OP1dB"))["data"] = .1
        with self.assertRaises(ValueError):
            comparison.compare(self.library, changed, 30, reverse_isolation_db=50, profile="antenna")


if __name__ == "__main__":
    unittest.main()
