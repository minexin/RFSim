"""Offline provenance, fixed acceptance threshold and noise-budget regression."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
path = Path(__file__).with_name("diagnose-attenuator-1db.py")
spec = importlib.util.spec_from_file_location("diagnostic", path)
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class OfflineDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.reference = diagnostic.load(ROOT / "validation/systemvue-2023-attenuator-1db.json")
        self.historical = diagnostic.load(
            ROOT / "validation/systemvue-2023-attenuator-1db-comparison.json"
        )
        self.evidence = diagnostic.load(
            ROOT / "validation/systemvue-2023-attenuator-offline-evidence.json"
        )
        self.actual = {c["measurement"]: c["rfmodel"] for c in self.historical["checks"]}

    def analyze(self):
        return diagnostic.analyze(self.reference, self.historical, self.evidence, self.actual)

    def test_original_failure_and_independent_math(self):
        result = self.analyze()
        self.assertFalse(result["vendor_subset_passed"])
        self.assertFalse(result["eligible_for_full_model_compatibility"])
        failed = [c["measurement"] for c in result["absolute_comparison"] if not c["passed"]]
        self.assertEqual(failed, ["output_noise_w_per_hz"])
        self.assertTrue(all(c["passed"] for c in result["probe_independent_checks"].values()))
        budget = result["path_noise_decomposition"]
        self.assertGreater(budget["source_only_remaining_relative_error"], 1e-7)
        self.assertAlmostEqual(
            budget["source_only_remaining_relative_error"], 2.0355423e-7, delta=1e-14
        )
        self.assertGreater(budget["source_offset_from_si_kT"], 9e-7)
        self.assertGreater(budget["inferred_added_offset_from_si"], 9e-7)
        self.assertLess(budget["example_constant_remaining_relative_error"], 1e-7)
        # A diagnostic hypothesis must not upgrade the immutable original failure.
        self.assertTrue(result["original_failure_preserved"])

    def test_thermal_budget_limits_and_temperature_reference(self):
        zero = diagnostic.independent_expected(0, 250, 1e-9)
        self.assertEqual(zero["gain"], 1)
        self.assertEqual(zero["noise_factor"], 1)
        self.assertEqual(zero["intrinsic_output_w_per_hz"], 0)
        hot = diagnostic.independent_expected(10, 350, 1e-9)
        self.assertAlmostEqual(hot["gain"], 0.1)
        self.assertAlmostEqual(hot["noise_factor"], 1 + 9 * 350 / 290)
        self.assertAlmostEqual(
            hot["source_output_w_per_hz"] / hot["intrinsic_output_w_per_hz"], 1 / 9
        )
        self.assertAlmostEqual(hot["output_noise_w_per_hz"] / (1.380649e-23 * 350), 1, places=14)

    def test_changed_threshold_or_history_rejected(self):
        for field, value in [
            ("relative_tolerance", 1e-6),
            ("systemvue", 1.0),
            ("rfmodel", 0.5),
            ("passed", False),
        ]:
            with self.subTest(field=field):
                saved = copy.deepcopy(self.historical)
                self.historical["checks"][0][field] = value
                with self.assertRaises(ValueError):
                    self.analyze()
                self.historical = saved
        self.historical["passed"] = True
        with self.assertRaises(ValueError):
            self.analyze()

    def test_units_and_parameter_readbacks(self):
        for value in [1.0, -1, float("nan"), True]:
            with self.subTest(loss=value):
                original = self.evidence["runtime_readbacks"]["L"]
                self.evidence["runtime_readbacks"]["L"] = value
                with self.assertRaises(ValueError):
                    self.analyze()
                self.evidence["runtime_readbacks"]["L"] = original
        self.evidence["runtime_readbacks"]["device_temperatures_c"] = [290]
        with self.assertRaises(ValueError):
            self.analyze()

    def test_evidence_identity_and_timestamp(self):
        for field, value in [
            ("model_help_page", "rfdesign/ATTN_NonLinear.html"),
            ("part_help_page", "rfdesign/Amp_(RF)_Part.html"),
            ("original_dataset_timestamp", "1"),
        ]:
            saved = self.evidence[field]
            self.evidence[field] = value
            with self.assertRaises(ValueError):
                self.analyze()
            self.evidence[field] = saved
        self.evidence["original_measurements"][2]["data"][1] *= 1.001
        with self.assertRaises(ValueError):
            self.analyze()

    def test_probe_mismatch_and_nonfinite_rejected(self):
        for value in [0.7, float("nan"), True]:
            self.actual["gain"] = value
            with self.assertRaises(ValueError):
                self.analyze()

    def test_spectral_integrity_and_distinct_reference_planes(self):
        report = self.analyze()
        self.assertEqual(len(report["spectral_peak_voltage_power_checks"]), 8)
        for budget in report["spectral_noise_budgets"]:
            self.assertTrue(budget["sum_check"]["passed"])
            self.assertAlmostEqual(budget["path_CND_over_raw_total"], -5e-9, delta=1e-14)
        self.evidence["spectral_terms"][0]["power_field"] *= 1.01
        with self.assertRaises(ValueError):
            self.analyze()

    def test_duplicate_or_missing_spectral_identity(self):
        self.evidence["spectral_terms"].append(copy.deepcopy(self.evidence["spectral_terms"][0]))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.analyze()
        self.evidence["spectral_terms"] = []
        with self.assertRaisesRegex(ValueError, "Missing"):
            self.analyze()

    def test_historical_files_cannot_be_output(self):
        p = ROOT / "validation/systemvue-2023-attenuator-1db-comparison.json"
        before = p.read_bytes()
        with self.assertRaisesRegex(ValueError, "overwrite"):
            diagnostic.write(p, {}, [])
        self.assertEqual(p.read_bytes(), before)

    @unittest.skipUnless(PROBE, "Native replay executable required")
    def test_cli_replay_and_failure_preserves_output(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            evidence = temp / "evidence.json"
            evidence.write_text(json.dumps(self.evidence), encoding="utf-8")
            output = temp / "result.json"
            command = [
                sys.executable,
                str(path),
                "replay",
                str(ROOT / "validation/systemvue-2023-attenuator-1db.json"),
                str(ROOT / "validation/systemvue-2023-attenuator-1db-comparison.json"),
                str(evidence),
                str(PROBE),
                str(output),
            ]
            passed = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(passed.returncode, 0, passed.stderr)
            report = diagnostic.load(output)
            self.assertFalse(report["vendor_subset_passed"])
            original = output.read_bytes()
            # Git CRLF/LF and JSON formatting differ across platforms; semantic values must not.
            reformatted = temp / "reference-lf.json"
            reformatted.write_bytes(json.dumps(self.reference, sort_keys=True).encode("utf-8"))
            command[3] = str(reformatted)
            repeat = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(repeat.returncode, 0, repeat.stderr)
            original = output.read_bytes()
            self.evidence["original_reference_canonical_sha256"] = "0" * 64
            evidence.write_text(json.dumps(self.evidence), encoding="utf-8")
            rejected = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
