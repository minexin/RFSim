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


if __name__ == "__main__":
    unittest.main()
