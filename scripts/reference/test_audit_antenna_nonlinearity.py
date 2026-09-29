import copy
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "audit", Path(__file__).with_name("audit-antenna-nonlinearity.py"))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class NonlinearAuditTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2]
        self.capture = json.loads((root / "validation/systemvue-2023-antenna-run-003.json").read_text())
        for device in ("RFAmp1", "RFAmp2"):
            for name, value in audit.EXPECTED.items():
                self.capture["nodes"].append({
                    "path": audit.antenna.parameter_path(device + "/" + name), "data": value})

    def test_valid_and_changed_nonlinear_parameters(self):
        self.assertEqual(len(audit.audit(self.capture, -50)["parameters"]), 16)
        for value in (123., float("nan"), True):
            capture = copy.deepcopy(self.capture)
            capture["nodes"][-1]["data"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                audit.audit(capture, -50)

    def test_missing_ambiguous_or_failed_read(self):
        for change in ("missing", "duplicate", "failed"):
            capture = copy.deepcopy(self.capture)
            if change == "missing":
                capture["nodes"].pop()
            elif change == "duplicate":
                capture["nodes"].append(copy.deepcopy(capture["nodes"][-1]))
            else:
                capture["nodes"][-1]["evaluation_error"] = "unavailable"
            with self.subTest(change=change), self.assertRaises(ValueError):
                audit.audit(capture, -50)


if __name__ == "__main__":
    unittest.main()
