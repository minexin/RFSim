import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-coherent-network.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()
captures_path = comparison.ROOT / "validation/systemvue-2023-coherent-network-captures.json"


class CoherentNetworkTests(unittest.TestCase):
    def setUp(self):
        self.captures = json.loads(captures_path.read_text(encoding="utf-8"))
        self.library = comparison.Library(library_path)

    def node(self, suffix):
        return next(n for n in self.captures[0]["nodes"] if n["path"].endswith(suffix))

    def test_real_capture_path_waves_and_merged_drive(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["reports"][0]["checks"]), 4)
        self.assertGreater(report["reports"][0]["checks"][2]["systemvue_w"], .0017)

    def test_incorrect_native_source_partition_fails_reference(self):
        with patch.object(self.library, "assign_source_coherence", return_value=(1, 2)):
            report = comparison.compare(self.library, self.captures)
        self.assertFalse(report["passed"])
        self.assertFalse(report["reports"][0]["checks"][2]["passed"])
        self.assertFalse(report["reports"][0]["checks"][3]["passed"])

    def test_phase_reversal_preserves_power_but_fails_complex_check(self):
        node = self.node("/V3")
        node["data"] = [-v for v in node["data"]]
        report = comparison.compare(self.library, self.captures)
        self.assertFalse(report["passed"])
        self.assertTrue(report["reports"][0]["checks"][2]["passed"])

    def test_merged_drive_change_fails(self):
        elements = self.node("/RFElemList")["data"]
        self.node("/RFPwrIn")["data"][elements.index("Attn1")] *= .5
        self.assertFalse(comparison.compare(self.library, self.captures)["passed"])

    def test_clock_identity_mismatch_rejected(self):
        node = self.node("/IDName")
        node["data"] = [value.replace("{1}", "{2}") if "MultiSource2" in value else value
                        for value in node["data"]]
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures)

    def test_stale_or_ambiguous_capture_rejected(self):
        self.node("/System1_Data")["timestamp"] = "0"
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures)
        self.setUp()
        self.captures.append(copy.deepcopy(self.captures[0]))
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures)

    def test_bad_complex_encoding_or_parameter_rejected(self):
        for mutation in ("shape", "bool", "length", "mode", "version"):
            self.setUp()
            if mutation == "shape":
                self.node("/V3")["dimensions"] = [4, 2]
            elif mutation == "bool":
                self.node("/V3")["data"][0] = True
            elif mutation == "length":
                self.captures[0]["line_length_rad"] = .5235987755982988
            elif mutation == "mode":
                self.node("/MultiSource1/ParamSet/SrcType")["data"] = 1
            else:
                self.captures[0]["systemvue_version"] = "unknown"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                comparison.compare(self.library, self.captures)


if __name__ == "__main__":
    unittest.main()
