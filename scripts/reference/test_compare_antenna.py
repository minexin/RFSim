import copy
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("antenna", Path(__file__).with_name("compare-antenna.py"))
antenna = importlib.util.module_from_spec(spec)
spec.loader.exec_module(antenna)


class AntennaTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[2] / "validation/systemvue-2023-antenna.json"
        self.reference = json.loads(path.read_text(encoding="utf-8"))

    def test_live_reference_and_stale_data(self):
        self.assertEqual(len(antenna.validate(self.reference)), 5)
        # Select dataset explicitly; node ordering is not part of the contract.
        for node in self.reference["nodes"]:
            if node["path"] == antenna.DATASET:
                node["timestamp"] = "1"
        with self.assertRaisesRegex(ValueError, "Stale"):
            antenna.validate(self.reference)

    def test_vectors_and_ambiguity(self):
        reference = copy.deepcopy(self.reference)
        reference["nodes"].append(copy.deepcopy(reference["nodes"][-1]))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            antenna.validate(reference)
        for node in self.reference["nodes"]:
            if node["path"].endswith("/CF"):
                node["data"][0] = 100e6
        with self.assertRaisesRegex(ValueError, "frequency"):
            antenna.validate(self.reference)

    def test_comparison_detects_gap(self):
        values = antenna.validate(self.reference)
        actual = {"nodes": [{"name": name, **{key: values[metric][i]
                   for metric, key in antenna.METRICS.items()}} for i, name in enumerate(antenna.NAMES)]}
        self.assertTrue(antenna.compare(self.reference, actual)["passed"])
        actual["nodes"][-1]["gain"] *= 1.00001
        self.assertFalse(antenna.compare(self.reference, actual)["passed"])
        actual["nodes"][-1]["gain"] = float("nan")
        with self.assertRaisesRegex(ValueError, "Invalid"):
            antenna.compare(self.reference, actual)

    def test_verified_parameters(self):
        path = Path(__file__).resolve().parents[2] / "validation/systemvue-2023-antenna-parameters.json"
        reference = json.loads(path.read_text(encoding="utf-8"))
        antenna.validate(reference)
        for node in reference["nodes"]:
            if node["path"] == antenna.parameter_path("RFAmp2/G"):
                node["data"] = 100
        with self.assertRaisesRegex(ValueError, "Parameter mismatch"):
            antenna.validate(reference)
        with self.assertRaisesRegex(ValueError, "parameter"):
            antenna.collect(self.reference)

    def test_source_alignment_uses_parameter_not_measurement(self):
        path = Path(__file__).resolve().parents[2] / "validation/systemvue-2023-antenna-parameters.json"
        reference = json.loads(path.read_text(encoding="utf-8"))
        expected = 1.3806503e-23 * 50
        self.assertAlmostEqual(antenna.aligned_source_density(reference) / expected, 1.)
        for node in reference["nodes"]:
            if node["path"].endswith("/CND"):
                node["data"][0] *= 2
        self.assertAlmostEqual(antenna.aligned_source_density(reference) / expected, 1.)
        with self.assertRaisesRegex(ValueError, "verified"):
            antenna.aligned_source_density(self.reference)

    def test_budget_conservation_and_names(self):
        actual = {"nodes": [{"name": "Source"}]}
        for index, name in enumerate(antenna.NAMES[1:], start=1):
            actual["nodes"].append({"name": name, "output_noise_w_per_hz": (index + 1) * 1e-21,
                                   "contributions": [{"name": part, "watts_per_hz": 1e-21}
                                                     for part in antenna.NAMES[:index + 1]]})
        result = antenna.noise_budget(actual)
        self.assertAlmostEqual(sum(x["fraction_of_total"] for x in result["nodes"][-1]["contributions"]), 1.)
        actual["nodes"][-1]["contributions"][0]["watts_per_hz"] *= 2
        with self.assertRaisesRegex(ValueError, "sum"):
            antenna.noise_budget(actual)
        actual["nodes"][-1]["contributions"].pop()
        with self.assertRaisesRegex(ValueError, "Missing"):
            antenna.noise_budget(actual)


if __name__ == "__main__":
    unittest.main()
