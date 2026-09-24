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

    def test_temperature_conditions(self):
        self.reference["parameters"]["temperature_k"] = 250
        self.assertEqual(len(comparison.validate_reference(self.reference, 1, 250)), 5)
        with self.assertRaises(ValueError):
            comparison.validate_reference(self.reference)
        for value in (0, -1, 1001, float("nan"), float("inf")):
            self.reference["parameters"]["temperature_k"] = value
            with self.assertRaisesRegex(ValueError, "Invalid temperature"):
                comparison.validate_reference(self.reference, 1, value)

    def test_source_power_conditions(self):
        self.reference["parameters"]["source_available_w"] = 1e-3
        self.assertEqual(len(comparison.validate_reference(self.reference, 1, 290, 1e-3)), 5)
        with self.assertRaises(ValueError):
            comparison.validate_reference(self.reference)
        for value in (0, -1, 1e-24, 2, float("nan"), float("inf")):
            self.reference["parameters"]["source_available_w"] = value
            with self.assertRaisesRegex(ValueError, "Invalid source power"):
                comparison.validate_reference(self.reference, 1, 290, value)

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
        with self.assertRaisesRegex(ValueError, "temperature"):
            collector.collect(capture, 1, 250)
        nodes[-2]["data"] = -23.15
        self.assertEqual(collector.collect(capture, 1, 250)["parameters"]["temperature_k"], 250)
        nodes[-2]["data"] = 16.85
        source = {"path": prefix + "Sch1/PartList/Source/ParamSet/Pwr", "data": 1e-3}
        nodes.append(source)
        self.assertEqual(collector.collect(capture, 1, 290, 0)["observed_source_available_w"], 1e-3)
        with self.assertRaisesRegex(ValueError, "source power"):
            collector.collect(capture, 1)
        nodes.pop()
        with self.assertRaisesRegex(ValueError, "did not take effect"):
            collector.collect(capture, 3)
        nodes.append(copy.deepcopy(nodes[0]))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            collector.collect(capture, 1)

    def test_residual_decomposition(self):
        residual_spec = importlib.util.spec_from_file_location(
            "residuals", Path(__file__).with_name("analyze-attenuator-residuals.py"))
        residuals = importlib.util.module_from_spec(residual_spec)
        residual_spec.loader.exec_module(residuals)
        sample = copy.deepcopy(self.reference)
        values = {item["path"].rsplit("/", 1)[-1]: item for item in sample["measurements"]}
        gain = 10 ** -0.1
        # A common source scale offset must cancel in the through-path ratio.
        values["DCP"]["data"] = [2e-19, 2e-19 * gain]
        values["CGAIN"]["data"] = [1., gain]
        values["CND"]["data"] = [5e-21, 5e-21]
        result = residuals.analyze(sample)
        self.assertAlmostEqual(result["signed_relative_residuals"]["source_signal_offset"], 1.)
        self.assertTrue(result["signal_transfer_within_1e_7"])
        self.assertTrue(result["noise_output_to_input_within_1e_7"])
        values["DCP"]["data"][1] *= 0.99
        self.assertFalse(residuals.analyze(sample)["signal_transfer_within_1e_7"])
        values["CND"]["data"][0] = 0
        with self.assertRaises(ValueError):
            residuals.analyze(sample)


if __name__ == "__main__":
    unittest.main()
