"""Rerun shared compression and reject misleading or incomplete evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

LIBRARY_PATH = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-shared-compression.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "validation/systemvue-2023-shared-compression-captures.json"


class SharedCompressionTests(unittest.TestCase):
    def setUp(self):
        self.captures = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.library = comparison.Library(LIBRARY_PATH)

    def test_real_direct_carriers_and_noise_control(self):
        report = comparison.compare(self.library, self.captures)
        self.assertEqual(report["warning_free_case_count"], 21)
        self.assertEqual(report["diagnostic_case_count"], 2)
        self.assertTrue(report["direct_carrier_passed"])
        self.assertFalse(report["drive_power_passed"])
        self.assertFalse(report["passed"])
        failed = [r for r in report["reports"] if not r["checks"][0]["passed"]]
        self.assertEqual(len(failed), 4)
        self.assertTrue(all(r["noise_enabled"] and r["source_powers_dbm"] == [-30., -30.] for r in failed))
        for case in failed:
            controls = [r for r in report["reports"] if not r["noise_enabled"] and
                        r["locked"] == case["locked"] and r["source_phases_deg"] == case["source_phases_deg"]]
            self.assertEqual(len(controls), 1)
            self.assertTrue(controls[0]["checks"][0]["passed"])
            excess = case["checks"][0]["systemvue_w"] - controls[0]["checks"][0]["systemvue_w"]
            self.assertAlmostEqual(excess, 1.601554432e-11, delta=1e-18)
        self.assertEqual(sum(len(c["relative_complex_errors"])
                             for r in report["reports"] for c in r["checks"][1:]), 64)

    def test_cancelled_output_absence_is_diagnostic_not_measured_zero(self):
        report = comparison.compare(self.library, self.captures)
        diagnostics = {d["noise_enabled"]: d for d in report["diagnostics"]}
        self.assertFalse(diagnostics[False]["output_spectrum_present"])
        self.assertTrue(diagnostics[True]["output_spectrum_present"])
        self.assertTrue(all(not d["affects_compatibility_verdict"] for d in diagnostics.values()))
        self.assertGreater(diagnostics[True]["systemvue_rfpwrin_w"], 1e-11)
        self.assertLess(diagnostics[False]["systemvue_rfpwrin_w"], 1e-30)
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [c for c in self.captures if c["diagnostic"]])

    def test_wrong_partition_or_phase_is_detected(self):
        capture = next(c for c in self.captures if c["locked"] and c["same_frequency"]
                       and not c["diagnostic"] and c["source_phases_deg"] == [0., 0.])
        with patch.object(self.library, "assign_source_coherence", return_value=(1, 2)):
            self.assertFalse(comparison.compare(self.library, [capture])["direct_carrier_passed"])
        voltage = comparison.two.node(capture, comparison.two.SPECTRUM + "V2")
        voltage["data"] = [-v for v in voltage["data"]]
        self.assertFalse(comparison.compare(self.library, [capture])["direct_carrier_passed"])

    def test_metadata_readbacks_shape_and_staleness_rejected(self):
        for mutation in ("noise", "clock", "frequency", "phase", "stale-main", "stale-path", "shape", "power"):
            capture = copy.deepcopy(self.captures[0])
            if mutation == "noise":
                capture["noise_enabled"] = not capture["noise_enabled"]
            elif mutation == "clock":
                capture["locked"] = not capture["locked"]
            elif mutation == "frequency":
                capture["same_frequency"] = not capture["same_frequency"]
            elif mutation == "phase":
                capture["source_phases_deg"] = [0., 45.]
            elif mutation.startswith("stale"):
                suffix = "System1_Data" if mutation == "stale-main" else "System1_Data_Folder/System1_Data_Path1"
                comparison.two.node(capture, suffix)["timestamp"] = "0"
            elif mutation == "shape":
                comparison.two.node(capture, comparison.two.SPECTRUM + "V2")["data"].pop()
            else:
                comparison.two.node(capture, comparison.two.SPECTRUM + "P2")["data"] = [True]*len(
                    comparison.two.vector(capture, "P2"))
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                comparison.compare(self.library, [capture])

    def test_partial_missing_spectrum_and_unknown_warning_rejected(self):
        noisy = next(c for c in self.captures if c["diagnostic"] and c["noise_enabled"])
        noisy["nodes"] = [n for n in noisy["nodes"] if not n["path"].endswith("/F2")]
        with self.assertRaises(ValueError):
            comparison.inspect(noisy)
        for text in ("", "(WARNING) Unrelated failure"):
            capture = copy.deepcopy(next(c for c in self.captures if c["diagnostic"]))
            capture["manager_errors"] = text
            with self.subTest(text=text), self.assertRaises(ValueError):
                comparison.inspect(capture)

    def test_duplicate_configuration_rejected(self):
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [self.captures[0], self.captures[0]])


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("archive", Path(__file__).with_name("archive-shared-compression.py"))
        self.archive = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.archive)
        self.record = json.loads(FIXTURE.read_text(encoding="utf-8"))[0]
        self.raw = copy.deepcopy(self.record)
        self.raw["nodes"].append({"path": comparison.two.BASE + "System1",
                                  "analysis_settings": self.record["analysis_settings"]})
        self.command = ["powershell.exe", "-RunCompressionAnalysis", "-CompressionTwoTone",
                        "-SourcePowerDbm", "-6", "-CompressionFirstPhaseDeg", "0",
                        "-CompressionSecondPhaseDeg", "90", "-CompressionShowTotals"]
        self.status = dict(command=self.command, case="compression", state="captured",
                           collector_exit_code=0, eligible_for_compatibility=True)

    def extract(self):
        return self.archive.extract(self.raw, self.status,
                                    raw_sha256=self.record["raw_capture_sha256"],
                                    workspace_sha256=self.record["source_workspace_sha256"],
                                    systemvue_version=self.record["systemvue_version"])

    def test_roundtrip_and_wrong_command_rejected(self):
        self.assertEqual(comparison.inspect(self.extract()), comparison.inspect(self.record))
        self.command.append("-CompressionSameFrequency")
        with self.assertRaises(ValueError):
            self.extract()

    def test_unfinished_failed_and_missing_settings_rejected(self):
        for change in ({"state": "timeout_unresolved"}, {"collector_exit_code": 1},
                       {"collector_exit_code": False}, {"eligible_for_compatibility": False}):
            self.setUp()
            self.status.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.extract()
        self.setUp()
        self.raw["nodes"].pop()
        with self.assertRaises(ValueError):
            self.extract()


if __name__ == "__main__":
    unittest.main()
