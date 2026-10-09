import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "comparison", Path(__file__).with_name("compare-coherent-network.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
library_path = Path(sys.argv.pop(1)).resolve()
captures_path = comparison.ROOT / "validation/systemvue-2023-coherent-network-captures.json"


class CoherentNetworkTests(unittest.TestCase):
    def setUp(self):
        self.captures = json.loads(captures_path.read_text(encoding="utf-8"))
        self.library = comparison.Library(library_path)

    def node(self, suffix):
        return next(n for n in self.captures[0]["nodes"] if n["path"].endswith(suffix))

    def test_real_capture_path_waves_and_merged_drive(self):
        report = comparison.compare(self.library, self.captures)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["reports"][0]["checks"]), 4)
        self.assertGreater(report["reports"][0]["checks"][2]["systemvue_w"], .0017)

    def test_new_phase_scan_preserves_the_observed_drive_gap(self):
        path = comparison.ROOT / "validation/systemvue-2023-coherent-phase-scan-captures.json"
        captures = json.loads(path.read_text(encoding="utf-8"))
        report = comparison.compare(self.library, captures)
        self.assertFalse(report["passed"])
        self.assertTrue(report["path_wave_and_clock_relation_passed"])
        self.assertFalse(report["rfpwrin_agreement_passed"])
        cases = {(r["locked"], r["second_phase_deg"]): r for r in report["reports"]}
        self.assertEqual(set(cases), {(locked, phase) for locked in (False, True) for phase in (0, 90, 180)})
        for key, case in cases.items():
            self.assertEqual(case["checks"][2]["passed"], key == (True, 0))
            self.assertGreater(case["checks"][2]["systemvue_w"], .0017)
        self.assertLess(cases[True, 180]["checks"][2]["rfmodel_w"], 1e-28)
        self.assertAlmostEqual(cases[False, 180]["checks"][2]["rfmodel_w"], .000888888888888889)

    def test_clear_cache_control_does_not_remove_drive_gap(self):
        path = comparison.ROOT / "validation/systemvue-2023-coherent-cache-control-captures.json"
        captures = json.loads(path.read_text(encoding="utf-8"))
        report = comparison.compare(self.library, captures)
        self.assertFalse(report["passed"])
        self.assertTrue(report["path_wave_and_clock_relation_passed"])
        self.assertFalse(report["rfpwrin_agreement_passed"])
        self.assertEqual(captures[0]["analysis_settings"]["CoherentIM"], 1)
        self.assertEqual(captures[0]["analysis_settings"]["ShowTotals"], 0)
        self.assertEqual(captures[0]["analysis_settings"]["CalcNoise"], 0)

    def test_incorrect_native_source_partition_fails_reference(self):
        with patch.object(self.library, "assign_source_coherence", return_value=(1, 2)):
            report = comparison.compare(self.library, self.captures)
        self.assertFalse(report["passed"])
        self.assertFalse(report["reports"][0]["checks"][2]["passed"])
        self.assertFalse(report["reports"][0]["checks"][3]["passed"])

    def test_phase_reversal_preserves_power_but_fails_complex_check(self):
        node = self.node("/V3")
        node["data"] = [-v for v in node["data"]]
        report = comparison.compare(self.library, self.captures)
        self.assertFalse(report["passed"])
        self.assertTrue(report["reports"][0]["checks"][2]["passed"])

    def test_merged_drive_change_fails(self):
        elements = self.node("/RFElemList")["data"]
        self.node("/RFPwrIn")["data"][elements.index("Attn1")] *= .5
        self.assertFalse(comparison.compare(self.library, self.captures)["passed"])

    def test_clock_identity_mismatch_rejected(self):
        node = self.node("/IDName")
        node["data"] = [value.replace("{1}", "{2}") if "MultiSource2" in value else value
                        for value in node["data"]]
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures)

    def test_stale_or_ambiguous_capture_rejected(self):
        self.node("/System1_Data")["timestamp"] = "0"
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures)
        self.setUp()
        self.captures.append(copy.deepcopy(self.captures[0]))
        with self.assertRaises(ValueError):
            comparison.compare(self.library, self.captures)

    def test_bad_complex_encoding_or_parameter_rejected(self):
        for mutation in ("shape", "bool", "length", "mode", "version"):
            self.setUp()
            if mutation == "shape":
                self.node("/V3")["dimensions"] = [4, 2]
            elif mutation == "bool":
                self.node("/V3")["data"][0] = True
            elif mutation == "length":
                self.captures[0]["line_length_rad"] = .5235987755982988
            elif mutation == "mode":
                self.node("/MultiSource1/ParamSet/SrcType")["data"] = 1
            else:
                self.captures[0]["systemvue_version"] = "unknown"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                comparison.compare(self.library, self.captures)


class CoherentCaptureArchiveTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location(
            "archive", Path(__file__).with_name("archive-coherent-captures.py"))
        self.archiver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.archiver)
        path = comparison.ROOT / "validation/systemvue-2023-coherent-phase-scan-captures.json"
        self.raw = json.loads(path.read_text(encoding="utf-8"))[0]
        self.status = {"case": "coherent", "state": "captured", "collector_exit_code": 0,
                       "eligible_for_compatibility": True,
                       "command": ["powershell.exe", "-RunCoherentAnalysis",
                                   "-CoherentPhaseDeg", "0", "-CoherentLengthRad",
                                   str(self.raw["line_length_rad"]), "-CoherentLocked"]}

    def extract(self):
        return self.archiver.extract(
            self.raw, self.status, raw_sha256=self.raw["raw_capture_sha256"],
            workspace_sha256=self.raw["source_workspace_sha256"],
            systemvue_version=self.raw["systemvue_version"])

    def test_roundtrip_preserves_actual_measurements(self):
        record = self.extract()
        self.assertEqual(comparison.inspect(record), comparison.inspect(self.raw))
        self.assertEqual(record["workspace_hash_scope"], "on_disk_source_copy_at_archive_time")

    def test_unfinished_or_failed_run_is_rejected(self):
        for change in ({"state": "timeout_unresolved"}, {"collector_exit_code": 1},
                       {"collector_exit_code": False}, {"eligible_for_compatibility": False}):
            self.setUp()
            self.status.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.extract()

    def test_command_intent_must_match_readbacks(self):
        for mode in ("phase", "clock", "missing-length", "duplicate"):
            self.setUp()
            command = self.status["command"]
            if mode == "phase":
                command[command.index("-CoherentPhaseDeg") + 1] = "90"
            elif mode == "clock":
                command.remove("-CoherentLocked")
            elif mode == "missing-length":
                index = command.index("-CoherentLengthRad")
                del command[index:index + 2]
            else:
                command.extend(["-CoherentPhaseDeg", "0"])
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.extract()


if __name__ == "__main__":
    unittest.main()
