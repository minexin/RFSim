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

    def test_loss_scan(self):
        path = Path(__file__).resolve().parents[2] / "validation/systemvue-2023-attenuator-loss-scan.json"
        scan = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual([sample["parameters"]["loss_db"] for sample in scan["samples"]], [0, 1, 3, 10])
        for sample in scan["samples"]:
            loss = sample["parameters"]["loss_db"]
            self.assertEqual(len(comparison.validate_reference(sample, loss)), 5)
        with self.assertRaises(ValueError):
            comparison.validate_reference(scan["samples"][-1], 1)
    def test_capture_collection(self):
        collector_spec = importlib.util.spec_from_file_location(
            "collector", Path(__file__).with_name("collect-attenuator-scan.py"))
        collector = importlib.util.module_from_spec(collector_spec)
        collector_spec.loader.exec_module(collector)
        prefix = "RFModel_AttenuatorNoise/Designs/"
        nodes = copy.deepcopy(self.reference["measurements"])
        nodes.extend([
            {"path": prefix + "Sch1/PartList/Attn/ParamSet/L", "data": 10 ** 0.1},
            {"path": prefix + "System1/RoomTemp", "data": 16.85},
            {"path": prefix + "System1_Data_Folder/System1_Sch1_Data_Path1",
             "timestamp": self.reference["dataset_timestamp"]},
        ])
        capture = {key: self.reference[key] for key in
                   ("run_started_utc", "run_returned_utc", "manager_errors")}
        capture["nodes"] = nodes
        self.assertEqual(collector.collect(capture, 1)["parameters"]["loss_db"], 1)
        with self.assertRaisesRegex(ValueError, "did not take effect"):
            collector.collect(capture, 3)
        nodes.append(copy.deepcopy(nodes[0]))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            collector.collect(capture, 1)


if __name__ == "__main__":
    unittest.main()
