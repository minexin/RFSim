import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("harmonics", Path(__file__).with_name("compare-single-harmonics.py"))
harmonics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harmonics)
library_path = Path(sys.argv.pop(1)).resolve()


class HarmonicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = harmonics.single.Library(library_path)
        cls.captures = json.loads((harmonics.single.ROOT / "validation" /
            "systemvue-2023-single-harmonic-captures.json").read_text())

    def test_low_power_agreement_and_compression_gap(self):
        low = harmonics.compare(self.library, self.captures[0], -30)
        high = harmonics.compare(self.library, self.captures[1], .9)
        self.assertTrue(low["passed"])
        self.assertFalse(high["passed"])
        self.assertEqual(len(low["checks"]), 2)
        self.assertGreater(high["checks"][0]["signed_relative_error"], .3)
        self.assertGreater(high["checks"][1]["signed_relative_error"], .5)

    def test_spectrum_ids_are_not_fixed_numbers(self):
        capture = copy.deepcopy(self.captures[0])
        for node in capture["nodes"]:
            if node["path"].endswith(("/IDNo", "/ID2")):
                node["data"] = [value + 1000 for value in node["data"]]
        self.assertTrue(harmonics.compare(self.library, capture, -30)["passed"])

    def test_common_drive_evidence_across_power_sweep(self):
        powers = set()
        diagnostics = {}
        for capture in self.captures:
            power = capture["source_power_dbm"]
            self.assertNotIn(power, powers)
            powers.add(power)
            report = harmonics.compare(self.library, capture, power)
            diagnostic = report["effective_drive_diagnostic"]
            self.assertFalse(diagnostic["affects_compatibility_verdict"])
            self.assertLess(abs(diagnostic["relative_disagreement"]), 1e-12)
            diagnostics[power] = diagnostic["from_h2"]
        self.assertEqual(powers, {-30, -10, -3, 0, .9})
        self.assertAlmostEqual(diagnostics[-30], diagnostics[-10], places=12)
        self.assertGreater(diagnostics[-10], diagnostics[-3])
        self.assertGreater(diagnostics[-3], diagnostics[0])
        self.assertGreater(diagnostics[0], diagnostics[.9])

    def test_changed_harmonic_breaks_common_drive_inference(self):
        capture = copy.deepcopy(self.captures[1])
        base = harmonics.single.BASE + "System1_Data/Eqns/VarBlock/"
        vectors = {n["path"][len(base):]: n for n in capture["nodes"] if n["path"].startswith(base)}
        identifier = next(i for i, name in zip(vectors["IDNo"]["data"], vectors["IDName"]["data"])
                          if "[3x(Source.Source1)],RFAmp" in name and not name.endswith("RFAmp,RFAmp"))
        for index, identity in enumerate(vectors["ID2"]["data"]):
            if identity == identifier:
                vectors["P2"]["data"][index] *= .8
        report = harmonics.compare(self.library, capture, .9)
        self.assertGreater(abs(report["effective_drive_diagnostic"]["relative_disagreement"]), .01)
        self.assertFalse(report["passed"])

    def test_rejects_ambiguous_identity_or_nonflat_spectrum(self):
        for kind in ("identity", "flat", "frequency", "stale"):
            capture = copy.deepcopy(self.captures[0])
            base = harmonics.single.BASE + "System1_Data/Eqns/VarBlock/"
            vectors = {n["path"][len(base):]: n for n in capture["nodes"] if n["path"].startswith(base)}
            identity = next(i for i, name in zip(vectors["IDNo"]["data"], vectors["IDName"]["data"])
                            if "[2x(Source.Source1)],RFAmp" in name and not name.endswith("RFAmp,RFAmp"))
            index = vectors["ID2"]["data"].index(identity)
            if kind == "identity":
                vectors["IDName"]["data"] = ["wrong" for _ in vectors["IDName"]["data"]]
            elif kind == "flat":
                vectors["P2"]["data"][index] *= 2
            elif kind == "frequency":
                vectors["F2"]["data"][index] += 1
            else:
                next(n for n in capture["nodes"] if n["path"].endswith("/System1_Data_Path1"))["timestamp"] = "1"
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                harmonics.compare(self.library, capture, -30)


if __name__ == "__main__":
    unittest.main()
