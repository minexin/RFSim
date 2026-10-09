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

    def phase_captures(self):
        return json.loads((comparison.two_tone.single.ROOT / "validation" /
                           "systemvue-2023-amplifier-phase-captures.json").read_text())

    def test_measured_complex_amplitudes_include_conjugation_and_cubic_sign(self):
        report = comparison.compare(self.library, self.phase_captures(), complex_amplitudes=True)
        self.assertTrue(report["passed"])
        self.assertEqual(report["comparison"], "complex_amplitude_and_power")
        checks = [c for r in report["reports"] for c in r["checks"]]
        self.assertEqual(len(checks), 64)
        self.assertLess(max(c["complex_relative_error"] for c in checks), 4e-8)
        self.assertLess(max(abs(c["phase_error_deg"]) for c in checks), 1e-10)
        # The nonzero phase case covers the negative-frequency conjugate explicitly.
        by_origin = {tuple(c["contributors"]): c for c in report["reports"][1]["checks"]}
        self.assertGreater(by_origin[(-10, 11)]["systemvue_amplitude"][0], 0)
        self.assertLess(by_origin[(-10, 11)]["systemvue_amplitude"][1], 0)
        self.assertLess(by_origin[(10, 10, 10)]["systemvue_amplitude"][1], 0)

    def test_phase_flip_passes_power_only_but_fails_complex_comparison(self):
        capture = self.phase_captures()[1]
        names = comparison.two_tone.vector(capture, "IDName")
        ids = comparison.two_tone.vector(capture, "IDNo")
        identifier = next(i for i, n in zip(ids, names) if n.endswith("[2x(Source.Source1)],RFAmp"))
        voltage = comparison.two_tone.vector(capture, "V2")
        for index, value in enumerate(comparison.two_tone.vector(capture, "ID2")):
            if value == identifier:
                voltage[2 * index] *= -1
                voltage[2 * index + 1] *= -1
        self.assertTrue(comparison.compare(self.library, [capture])["passed"])
        report = comparison.compare(self.library, [capture], complex_amplitudes=True)
        self.assertFalse(report["passed"])
        failed = [c for c in report["reports"][0]["checks"] if not c["passed"]]
        self.assertEqual(len(failed), 1)
        self.assertEqual(failed[0]["contributors"], [10, 10])
        self.assertGreater(failed[0]["complex_relative_error"], 1.9)

    def test_complex_encoding_and_voltage_power_consistency_are_required(self):
        for mutation in ("missing", "shape", "boolean", "nonfinite", "impedance", "power", "nonflat"):
            capture = self.phase_captures()[1]
            voltage = comparison.two_tone.node(capture, comparison.two_tone.SPECTRUM + "V2")
            ids = comparison.two_tone.vector(capture, "ID2")
            names = dict(zip(comparison.two_tone.vector(capture, "IDNo"),
                             comparison.two_tone.vector(capture, "IDName")))
            index = next(i for i, v in enumerate(ids) if names[v].endswith("[2x(Source.Source1)],RFAmp"))
            if mutation == "missing":
                capture["nodes"].remove(voltage)
            elif mutation == "shape":
                voltage["dimensions"] = [2, len(voltage["data"]) // 2]
            elif mutation == "boolean":
                voltage["data"][2 * index] = True
            elif mutation == "nonfinite":
                voltage["data"][2 * index] = float("nan")
            elif mutation == "impedance":
                comparison.two_tone.vector(capture, "Z2")[2 * index + 1] = 1
            elif mutation == "power":
                voltage["data"][2 * index] *= 2
            else:
                voltage["data"][2 * index] *= -1
                voltage["data"][2 * index + 1] *= -1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                comparison.compare(self.library, [capture], complex_amplitudes=True)

    def test_phase_metadata_is_checked_against_evaluated_radians(self):
        for phases in (None, [], [30], [True, -45], [float("inf"), -45], [361, -45], [0, 0]):
            capture = self.phase_captures()[1]
            capture["source_phases_deg"] = phases
            with self.subTest(phases=phases), self.assertRaises(ValueError):
                comparison.compare(self.library, [capture], complex_amplitudes=True)

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
