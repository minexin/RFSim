import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("diagnostic", Path(__file__).with_name("diagnose-total-drive.py"))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)
library_path = Path(sys.argv.pop(1)).resolve()


class TotalDriveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = diagnostic.Library(library_path)
        cls.captures = json.loads((diagnostic.ROOT / "validation" /
            "systemvue-2023-antenna-total-drive-captures.json").read_text())

    def test_measured_total_drive_explains_most_residual(self):
        for capture in self.captures:
            report = diagnostic.diagnose(self.library, capture, capture["source_power_dbm"])
            self.assertGreater(report["additional_drive_w"], 0)
            self.assertGreater(report["fundamental_only_relative_residual"], 1e-6)
            self.assertLess(abs(report["total_drive_relative_residual"]), 1e-8)

    def test_rejects_invalid_total_power_and_element_mapping(self):
        for kind in ("low", "nan", "count", "duplicate", "p1db"):
            capture = copy.deepcopy(self.captures[0])
            nodes = {node["path"].split("/")[-1]: node for node in capture["nodes"]}
            if kind == "low":
                nodes["RFPwrIn"]["data"][-1] = 1e-9
            elif kind == "nan":
                nodes["RFPwrIn"]["data"][-1] = float("nan")
            elif kind == "count":
                nodes["RFPwrIn"]["data"].pop()
            elif kind == "duplicate":
                nodes["RFElemList"]["data"][-1] = "RFAmp1"
            else:
                nodes["OP1dB"]["data"] = 1
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                diagnostic.diagnose(self.library, capture, 10)


if __name__ == "__main__":
    unittest.main()
