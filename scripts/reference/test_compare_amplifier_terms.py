import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-amplifier-terms.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class MixingTermTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.two_tone.single.Library(library_path)
        cls.captures = json.loads((comparison.two_tone.single.ROOT / "validation" /
                                  "systemvue-2023-two-tone-captures.json").read_text())

    def test_explicit_power_pair_replays_equal_reference(self):
        capture = copy.deepcopy(self.captures[0])
        del capture["source_power_dbm_per_tone"]
        capture["source_powers_dbm"] = [-30, -30]
        report = comparison.compare(self.library, [capture])
        self.assertTrue(report["passed"])
        self.assertEqual(report["reports"][0]["source_powers_dbm"], [-30, -30])

    def test_power_metadata_rejects_ambiguity_and_duplicate_pairs(self):
        for pair in ([], [-30], [-30, -30, -30], [-30, True], [-30, "-30"], [-30, float("nan")]):
            capture = copy.deepcopy(self.captures[0])
            del capture["source_power_dbm_per_tone"]
            capture["source_powers_dbm"] = pair
            with self.subTest(pair=pair), self.assertRaises(ValueError):
                comparison.compare(self.library, [capture])
        capture = copy.deepcopy(self.captures[0])
        capture["source_powers_dbm"] = [-30, -40]
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])
        capture["source_powers_dbm"] = [-30, -30]
        del capture["source_power_dbm_per_tone"]
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture, self.captures[0]])

    def test_all_48_terms_including_twelve_carrier_products(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        checks = [c for r in report["reports"] for c in r["checks"]]
        self.assertEqual(len(checks), 48)
        self.assertEqual(sum(c["carrier_overlap"] for c in checks), 12)

    def test_measured_unequal_cases_and_swapped_intermodulation(self):
        captures = json.loads((comparison.two_tone.single.ROOT / "validation" /
                               "systemvue-2023-unequal-two-tone-captures.json").read_text())
        report = comparison.compare(self.library, captures)
        self.assertTrue(report["passed"])
        self.assertEqual([r["source_powers_dbm"] for r in report["reports"]],
                         [[-30, -40], [-3, -12], [-12, -3], [0, -20]])
        self.assertEqual(sum(len(r["checks"]) for r in report["reports"]), 64)
        # A 9 dB input imbalance gives a 9 dB IM3 sideband imbalance, reversed on swapping.
        for index, ratio in ((1, 7.943282347242816), (2, 0.12589254117941673)):
            checks = {tuple(c["contributors"]): c for c in report["reports"][index]["checks"]}
            for field in ("systemvue_power_w", "rfmodel_power_w"):
                actual = checks[(-11, 10, 10)][field] / checks[(-10, 11, 11)][field]
                self.assertAlmostEqual(actual, ratio, places=10)

    def test_unequal_metadata_cannot_be_swapped_or_relabelled_equal(self):
        captures = json.loads((comparison.two_tone.single.ROOT / "validation" /
                               "systemvue-2023-unequal-two-tone-captures.json").read_text())
        for pair in ([-12, -3], [-3, -3]):
            capture = copy.deepcopy(captures[1])
            capture["source_powers_dbm"] = pair
            with self.subTest(pair=pair), self.assertRaises(ValueError):
                comparison.compare(self.library, [capture])

    def test_independent_profile_matches_48_measured_terms(self):
        captures = json.loads((comparison.two_tone.single.ROOT / "validation" /
                               "systemvue-2023-limiter-two-tone-captures.json").read_text())
        report = comparison.compare(self.library, captures)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["reports"]), 3)
        self.assertTrue(all(r["profile"] == "limiter" and len(r["checks"]) == 16
                            for r in report["reports"]))
        small = captures[0]
        for profile in ("sample", "antenna", "unknown"):
            changed = copy.deepcopy(small)
            changed["profile"] = profile
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                comparison.compare(self.library, [changed])

    def test_equal_power_pair_in_distinct_profiles_is_not_a_duplicate(self):
        root = comparison.two_tone.single.ROOT / "validation"
        sample = json.loads((root / "systemvue-2023-unequal-two-tone-captures.json").read_text())[0]
        limiter = json.loads((root / "systemvue-2023-limiter-two-tone-captures.json").read_text())[0]
        report = comparison.compare(self.library, [sample, limiter])
        self.assertTrue(report["passed"])
        self.assertEqual([r["profile"] for r in report["reports"]], ["sample", "limiter"])
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [limiter, limiter])

    def test_changed_overlap_power_fails_without_changing_other_terms(self):
        capture = copy.deepcopy(self.captures[-1])
        identities = comparison.two_tone.vector(capture, "IDNo")
        names = comparison.two_tone.vector(capture, "IDName")
        identifier = next(i for i, n in zip(identities, names)
                          if n.endswith("[-(Source.Source1)+2x(Source.Source1)],RFAmp"))
        values = comparison.two_tone.vector(capture, "P2")
        for index, value in enumerate(comparison.two_tone.vector(capture, "ID2")):
            if value == identifier:
                values[index] *= 1.01
        report = comparison.compare(self.library, [capture])
        self.assertFalse(report["passed"])
        failed = [c for c in report["reports"][0]["checks"] if not c["passed"]]
        self.assertEqual(len(failed), 1)
        self.assertEqual(failed[0]["contributors"], [-10, 10, 10])

    def test_wrong_origin_is_not_matched_by_frequency_alone(self):
        capture = copy.deepcopy(self.captures[0])
        names = comparison.two_tone.vector(capture, "IDName")
        for index, name in enumerate(names):
            if name.endswith("[-(Source.Source1)+2x(Source.Source1)],RFAmp"):
                names[index] = name.replace("Source.Source1", "Unrelated.Source1")
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])


if __name__ == "__main__":
    unittest.main()
