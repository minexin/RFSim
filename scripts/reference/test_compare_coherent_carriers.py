import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-coherent-carriers.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()


class CoherentCarrierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.terms.two_tone.single.Library(library_path)
        cls.captures = json.loads((comparison.terms.two_tone.single.ROOT / "validation" /
                                  "systemvue-2023-amplifier-phase-captures.json").read_text())

    def test_eight_measured_totals_preserve_incoherent_carrier_products(self):
        result = comparison.compare(self.library, self.captures)
        self.assertTrue(result["passed"])
        self.assertEqual(sum(len(r["checks"]) for r in result["reports"]), 8)

    def test_changed_node_total_is_not_reconstructed_from_its_own_parts(self):
        capture = copy.deepcopy(self.captures[0])
        vector = lambda name: comparison.terms.two_tone.vector(capture, name)
        names = dict(zip(vector("IDNo"), vector("IDName")))
        for index, (f, identifier) in enumerate(zip(vector("F2"), vector("ID2"))):
            if f == 1e9 and names[identifier] == "Node Total from 'RFAmp'":
                vector("P2")[index] *= 1.01
        result = comparison.compare(self.library, [capture])
        self.assertFalse(result["passed"])
        self.assertFalse(result["reports"][0]["checks"][0]["passed"])
        self.assertTrue(result["reports"][0]["checks"][1]["passed"])

    def test_changed_coherency_numbers_cannot_silently_merge_carrier_terms(self):
        capture = copy.deepcopy(self.captures[0])
        names = comparison.terms.two_tone.vector(capture, "IDName")
        for index, name in enumerate(names):
            if name.endswith("[-(Source.Source2)+(Source.Source1)+(Source.Source2)],RFAmp") or name.endswith(
                    "[-(Source.Source1)+2x(Source.Source1)],RFAmp"):
                names[index] = "{999}" + name.split("}", 1)[1]
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])

    def test_noise_dominated_total_cannot_validate_signal_only_reduction(self):
        capture = copy.deepcopy(self.captures[0])
        vector = lambda name: comparison.terms.two_tone.vector(capture, name)
        names = dict(zip(vector("IDNo"), vector("IDName")))
        for index, identifier in enumerate(vector("ID2")):
            if names[identifier] == "Node Noise from 'RFAmp'":
                vector("P2")[index] = 1e-3
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [capture])


if __name__ == "__main__":
    unittest.main()
