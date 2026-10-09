"""Real-capture replay plus failures that must not produce a compatibility pass."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-coherent-amplifier.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class CoherentAmplifierReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.shared.Library(LIBRARY)
        cls.original = json.loads(
            (ROOT / "validation/systemvue-2023-shared-compression-captures.json").read_text())

    def setUp(self):
        self.captures = copy.deepcopy(self.original)

    def test_complete_complex_spectra_and_group_partitions(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["reports"]), 21)
        self.assertEqual(sum(len(r["checks"]) for r in report["reports"]), 206)
        self.assertEqual(len(report["diagnostics"]), 2)
        for r in report["reports"]:
            self.assertTrue(r["group_partition_passed"])
            expected = 4 if r["locked"] else 15 if r["same_frequency"] else 16
            self.assertEqual(len(r["checks"]), expected)

    def test_invalid_provenance_clock_and_stale_datasets_rejected(self):
        for change in ("clock", "version", "script", "messages", "stale"):
            c = copy.deepcopy(self.captures[0])
            if change == "clock":
                c["locked"] = True
            elif change == "version":
                c["systemvue_version"] = "2024"
            elif change == "script":
                c.pop("script_language")
            elif change == "messages":
                c["manager_errors"] = "Unexpected simulation failure"
            else:
                comparison.shared.two.node(c, "System1_Data")["timestamp"] = "0"
            with self.subTest(change=change), self.assertRaises((ValueError, KeyError)):
                comparison.compare(self.library, [c])

    def test_missing_native_products_and_wrong_kind_rejected(self):
        original = self.library.coherent_amplifier
        for mutation in ("missing", "kind", "duplicate"):
            def corrupt(*args, **kwargs):
                result = original(*args, **kwargs)
                if mutation == "missing":
                    terms = result.terms[:-1]
                elif mutation == "duplicate":
                    terms = result.terms + result.terms[:1]
                else:
                    term = result.terms[-1]
                    term = term._replace(component=term.component._replace(kind=comparison.SpectrumKind.SOURCE))
                    terms = result.terms[:-1] + (term,)
                return result._replace(terms=terms)
            with self.subTest(mutation=mutation), patch.object(
                    self.library, "coherent_amplifier", side_effect=corrupt), self.assertRaises(ValueError):
                comparison.compare(self.library, self.captures[:1])

    def test_phase_and_group_errors_fail_numeric_verdict(self):
        original = self.library.coherent_amplifier
        for mutation in ("phase", "group"):
            def corrupt(*args, **kwargs):
                result = original(*args, **kwargs)
                term = result.terms[-1]
                value = term.component
                changed = value._replace(amplitude=-value.amplitude) if mutation == "phase" else (
                    value._replace(coherence_group=result.terms[-2].component.coherence_group))
                return result._replace(terms=result.terms[:-1] + (term._replace(component=changed),))
            with self.subTest(mutation=mutation), patch.object(
                    self.library, "coherent_amplifier", side_effect=corrupt):
                self.assertFalse(comparison.compare(self.library, self.captures[:1])["passed"])

    def test_duplicate_and_diagnostic_only_not_accepted(self):
        for values in ([], self.captures[:1]*2, [c for c in self.captures if c["diagnostic"]]):
            with self.assertRaises(ValueError):
                comparison.compare(self.library, values)


if __name__ == "__main__":
    unittest.main()
