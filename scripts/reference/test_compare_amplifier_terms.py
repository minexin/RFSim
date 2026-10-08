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

    def test_all_48_terms_including_twelve_carrier_products(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        checks = [c for r in report["reports"] for c in r["checks"]]
        self.assertEqual(len(checks), 48)
        self.assertEqual(sum(c["carrier_overlap"] for c in checks), 12)

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
