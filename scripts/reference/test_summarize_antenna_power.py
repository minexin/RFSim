import copy
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "power", Path(__file__).with_name("summarize-antenna-power.py"))
power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(power)


class PowerSummaryTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2] / "validation"
        self.captures = [json.loads((root / name).read_text(encoding="utf-8")) for name in (
            "systemvue-2023-antenna-minus60.json", "systemvue-2023-antenna-run-003.json")]

    def test_sort_and_physical_power_normalization(self):
        result = power.summarize(list(reversed(self.captures)))
        self.assertEqual([point["source_power_dbm"] for point in result["points"]], [-60., -50.])
        baseline, higher = result["points"]
        self.assertEqual(baseline["nodes"][-1]["gain_change_fraction_from_lowest_power"], 0)
        self.assertLess(higher["nodes"][-1]["gain_change_fraction_from_lowest_power"], 0)
        self.assertAlmostEqual(higher["nodes"][0]["signal_transducer_gain"], 1., places=6)

    def test_reject_duplicate_unverified_and_changed_noise_source(self):
        with self.assertRaises(ValueError):
            power.summarize([self.captures[0], self.captures[0]])
        for change in ("unverified", "noise", "digest"):
            captures = copy.deepcopy(self.captures)
            if change == "unverified":
                captures[0]["parameters_verified"] = False
            elif change == "digest":
                captures[0]["capture_sha256"] = "invalid"
            else:
                node = next(node for node in captures[0]["nodes"]
                            if node["path"] == power.antenna.parameter_path("Source/NoisePower"))
                node["data"][1] *= 2
            with self.subTest(change=change), self.assertRaises(ValueError):
                power.summarize(captures)


if __name__ == "__main__":
    unittest.main()
