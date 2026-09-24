import copy
import importlib.util
import json
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "compare_attenuator", Path(__file__).with_name("compare-attenuator.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class ReferenceValidationTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[2] / "validation/systemvue-2023-attenuator-1db.json"
        self.reference = json.loads(path.read_text(encoding="utf-8-sig"))

    def test_captured_reference(self):
        self.assertEqual(len(comparison.validate_reference(self.reference)), 5)

    def test_stale_dataset(self):
        self.reference["dataset_timestamp"] = "1"
        with self.assertRaisesRegex(ValueError, "timestamp"):
            comparison.validate_reference(self.reference)

    def test_invalid_vectors(self):
        for bad_data in ([1.], [1., float("nan")]):
            reference = copy.deepcopy(self.reference)
            reference["measurements"][0]["data"] = bad_data
            with self.assertRaises(ValueError):
                comparison.validate_reference(reference)
        self.reference["measurements"].pop()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            comparison.validate_reference(self.reference)

    def test_wrong_conditions_and_runtime_error(self):
        self.reference["parameters"]["loss_db"] = 2
        with self.assertRaises(ValueError):
            comparison.validate_reference(self.reference)
        self.reference["parameters"]["loss_db"] = 1
        self.reference["manager_errors"] = "analysis failed"
        with self.assertRaises(ValueError):
            comparison.validate_reference(self.reference)


if __name__ == "__main__":
    unittest.main()
