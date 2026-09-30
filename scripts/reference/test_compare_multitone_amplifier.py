import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-multitone-amplifier.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class NativeMultiToneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.two_tone.single.Library(library_path)
        cls.captures = json.loads((comparison.two_tone.single.ROOT / "validation" /
                                  "systemvue-2023-two-tone-captures.json").read_text())

    def test_native_matches_36_powers_with_original_gaps_preserved(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        self.assertEqual(sum(len(r["checks"]) for r in report["reports"]), 36)
        self.assertEqual([r["unlimited_passed"] for r in report["reports"]], [True, False, False])
        self.assertTrue(all(not r["manager_messages"] for r in report["reports"]))

    def test_modified_observations_do_not_change_native_prediction(self):
        capture = copy.deepcopy(self.captures[-1])
        original = comparison.compare(self.library, [capture])
        entry = comparison.two_tone.node(capture, comparison.two_tone.SPECTRUM + "P2")
        entry["data"] = [p * 1.01 for p in entry["data"]]
        changed = comparison.compare(self.library, [capture])
        self.assertFalse(changed["passed"])
        for before, after in zip(original["reports"][0]["checks"], changed["reports"][0]["checks"]):
            self.assertEqual(before["rfmodel_power_w"], after["rfmodel_power_w"])

    def test_warning_and_duplicate_cases_reject(self):
        capture = copy.deepcopy(self.captures[0])
        capture["manager_errors"] = "(WARNING) not accepted"
        for captures in ([capture], [], self.captures + [self.captures[0]]):
            with self.assertRaises(ValueError):
                comparison.compare(self.library, captures)


if __name__ == "__main__":
    unittest.main()
