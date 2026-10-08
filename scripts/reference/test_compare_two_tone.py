import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-two-tone.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class TwoToneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.single.Library(library_path)
        cls.captures = json.loads((comparison.single.ROOT / "validation" /
                                  "systemvue-2023-two-tone-captures.json").read_text())

    def test_unequal_source_parameters_use_ordered_powers_and_total_limit(self):
        # Parameter-only fixture: no spectrum observations are changed or compared.
        capture = copy.deepcopy(self.captures[0])
        comparison.node(capture, "Sch1/PartList/Source/ParamSet/Pwr")["data"] = [1e-6, 1e-7]
        power, _ = comparison.inspect(capture, -30, second_power_dbm=-40)
        self.assertAlmostEqual(power, 1e-6)
        with self.assertRaises(ValueError):
            comparison.inspect(capture, -40, second_power_dbm=-30)
        with self.assertRaises(ValueError):
            comparison.inspect(capture, -30)
        for second in (float("nan"), float("inf"), True, -201, 30):
            with self.subTest(second=second), self.assertRaises(ValueError):
                comparison.inspect(capture, -30, second_power_dbm=second)

    def test_measured_small_signal_passes_and_compression_gaps_remain(self):
        reports = [comparison.compare(self.library, c, c["source_power_dbm_per_tone"])
                   for c in self.captures]
        self.assertEqual([r["passed"] for r in reports], [True, False, False])
        self.assertTrue(all(len(r["checks"]) == 10 for r in reports))
        self.assertTrue(all(c["passed"] for r in reports for c in r["direct_fundamental_checks"]))
        for report in reports:
            total = report["limiter_hypotheses"]["total_rf"]
            self.assertFalse(total["affects_compatibility_verdict"])
            self.assertLess(max(abs(c["signed_relative_error"]) for c in total["checks"]), 1e-7)
        self.assertGreater(max(abs(c["signed_relative_error"]) for c in
                               reports[-1]["limiter_hypotheses"]["per_tone"]["checks"]), .25)

    def test_identifiers_are_remapped_by_source_expression(self):
        capture = copy.deepcopy(self.captures[0])
        for name in ("IDNo", "ID2"):
            entry = comparison.node(capture, comparison.SPECTRUM + name)
            entry["data"] = [i + 10000 for i in entry["data"]]
        self.assertTrue(comparison.compare(self.library, capture, -30)["passed"])

    def test_parameters_shapes_and_channel_are_checked(self):
        for suffix, value in (("Source/ParamSet/Pwr", [1e-6, 2e-6]),
                              ("Source/ParamSet/Freq", [1e9, 1.2e9]),
                              ("Source/ParamSet/Phase", [0, 90]),
                              ("Source/ParamSet/SrcType", [0, 1]),
                              ("RFAmp/ParamSet/G", 10)):
            capture = copy.deepcopy(self.captures[0])
            comparison.node(capture, "Sch1/PartList/" + suffix)["data"] = value
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                comparison.compare(self.library, capture, -30)
        capture = copy.deepcopy(self.captures[0])
        comparison.node(capture, "System1/Path0/PathFreq")["data"] = 1.1e9
        with self.assertRaises(ValueError):
            comparison.compare(self.library, capture, -30)
        capture = copy.deepcopy(self.captures[0])
        comparison.node(capture, "Sch1/PartList/Source/ParamSet/Pwr")["dimensions"] = [2]
        with self.assertRaises(ValueError):
            comparison.compare(self.library, capture, -30)

    def test_duplicate_or_nonflat_products_and_stale_runs_reject(self):
        for mutation in ("duplicate", "nonflat", "stale", "stale_spectrum", "warning"):
            capture = copy.deepcopy(self.captures[0])
            if mutation == "duplicate":
                capture["nodes"].append(copy.deepcopy(comparison.node(capture, comparison.SPECTRUM + "IDName")))
            elif mutation == "nonflat":
                ids = comparison.vector(capture, "IDNo")
                names = comparison.vector(capture, "IDName")
                identifier = next(i for i, n in zip(ids, names) if n.endswith("[2x(Source.Source1)],RFAmp"))
                index = comparison.vector(capture, "ID2").index(identifier)
                comparison.vector(capture, "P2")[index] *= 2
            elif mutation == "stale":
                comparison.node(capture, "System1_Data_Folder/System1_Data_Path1")["timestamp"] = "1"
            elif mutation == "stale_spectrum":
                comparison.node(capture, "System1_Data")["timestamp"] = "1"
            else:
                capture["manager_errors"] = "(WARNING) unexpected"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                comparison.compare(self.library, capture, -30)

    def test_changed_powers_are_not_fitted_and_single_tone_rejects(self):
        capture = copy.deepcopy(self.captures[0])
        entry = comparison.node(capture, comparison.SPECTRUM + "P2")
        entry["data"] = [p * 1.01 for p in entry["data"]]
        report = comparison.compare(self.library, capture, -30)
        self.assertFalse(report["passed"])
        self.assertGreater(max(abs(c["signed_relative_error"]) for c in
                               report["limiter_hypotheses"]["total_rf"]["checks"]), .009)
        with self.assertRaises(ValueError):
            comparison.single.inspect_capture(self.captures[0], -30)


if __name__ == "__main__":
    unittest.main()
