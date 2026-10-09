import importlib.util
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

spec = importlib.util.spec_from_file_location(
    "runner", Path(__file__).with_name("run-systemvue-reference.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def test_coherent_options_reject_wrong_case_and_nonfinite_values(self):
        cases = [("compression", ["--coherent-locked"]),
                 ("attenuator", ["--coherent-show-totals"]),
                 ("coherent", ["--source-power-dbm", "0"])]
        for flag in ("--coherent-phase-deg", "--coherent-length-rad"):
            cases.extend([("coherent", [flag + "=" + value]) for value in ("nan", "inf", "361")])
            cases.append(("antenna", [flag, "30"]))
        for case, options in cases:
            with self.subTest(case=case, options=options):
                with patch.object(sys, "argv", ["runner", case, "unused.wsv", "unused", *options]):
                    with contextlib.redirect_stderr(io.StringIO()), patch.object(runner, "execute") as execute:
                        with self.assertRaises(SystemExit) as failure:
                            runner.main()
                        self.assertEqual(failure.exception.code, 2)
                        execute.assert_not_called()

    @unittest.skipUnless(sys.platform == "win32", "Valid COM launch requires Windows paths")
    def test_coherent_options_reach_collector(self):
        root = Path(__file__).resolve().parents[2]
        (root / "build-reference").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root / "build-reference") as directory:
            workspace = Path(directory) / "RFModel_PhaseCombiner.wsv"
            workspace.touch()
            args = ["runner", "coherent", str(workspace), str(Path(directory) / "output"),
                    "--coherent-locked", "--coherent-show-totals", "--coherent-phase-deg", "90", "--coherent-length-rad", "30"]
            with patch.object(sys, "argv", args), patch.object(runner, "execute", return_value=0) as execute:
                self.assertEqual(runner.main(), 0)
            command = execute.call_args.args[0]
            self.assertIn("-RunCoherentAnalysis", command)
            self.assertIn("-CoherentLocked", command)
            self.assertIn("-CoherentShowTotals", command)
            self.assertEqual(command[command.index("-CoherentPhaseDeg") + 1], "90.0")
            self.assertEqual(command[command.index("-CoherentLengthRad") + 1], "30.0")

    @unittest.skipUnless(sys.platform == "win32", "Valid COM launch requires Windows paths")
    def test_second_tone_power_reaches_collector_command(self):
        # Only replace process launch; parsing, path guards and command construction are real.
        root = Path(__file__).resolve().parents[2]
        (root / "build-reference").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root / "build-reference") as directory:
            workspace = Path(directory) / "RFModel_AmplifierCompression.wsv"
            workspace.touch()
            arguments = ["runner", "compression", str(workspace), str(Path(directory) / "output"),
                         "--compression-two-tone", "--source-power-dbm", "-3",
                         "--compression-second-power-dbm", "-12", "--compression-profile", "limiter",
                         "--compression-first-phase-deg", "30", "--compression-second-phase-deg", "-45"]
            with patch.object(sys, "argv", arguments), patch.object(runner, "execute", return_value=0) as execute:
                self.assertEqual(runner.main(), 0)
            command = execute.call_args.args[0]
            self.assertEqual(command[command.index("-CompressionFirstPhaseDeg") + 1], "30.0")
            self.assertEqual(command[command.index("-CompressionSecondPhaseDeg") + 1], "-45.0")
            self.assertEqual(command[command.index("-CompressionProfile") + 1], "limiter")
            self.assertEqual(command[command.index("-SourcePowerDbm") + 1], "-3.0")
            self.assertEqual(command[command.index("-CompressionSecondPowerDbm") + 1], "-12.0")

    def test_phase_options_reject_nonfinite_out_of_range_and_single_tone(self):
        for flag in ("--compression-first-phase-deg", "--compression-second-phase-deg"):
            for value, mode in (("30", []), ("nan", ["--compression-two-tone"]),
                                ("inf", ["--compression-two-tone"]),
                                ("361", ["--compression-two-tone"])):
                args = ["runner", "compression", "unused.wsv", "unused-output",
                        "--source-power-dbm", "-3", flag + "=" + value, *mode]
                with self.subTest(flag=flag, value=value), patch.object(sys, "argv", args):
                    with contextlib.redirect_stderr(io.StringIO()), patch.object(runner, "execute") as execute:
                        with self.assertRaises(SystemExit) as failure:
                            runner.main()
                        self.assertEqual(failure.exception.code, 2)
                        execute.assert_not_called()

    def test_second_power_rejects_invalid_or_single_tone_requests(self):
        cases = [["--compression-second-power-dbm", "-12"]]
        cases.extend([["--compression-two-tone", "--compression-second-power-dbm=" + value]
                      for value in ("nan", "inf", "-201", "31")])
        for options in cases:
            arguments = ["runner", "compression", "unused.wsv", "unused-output",
                         "--source-power-dbm", "-3", *options]
            with self.subTest(options=options), patch.object(sys, "argv", arguments):
                with patch.object(runner, "execute") as execute, contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as failure:
                        runner.main()
                    self.assertEqual(failure.exception.code, 2)
                    execute.assert_not_called()

    def test_two_tone_rejects_ambiguous_or_incompatible_options(self):
        cases = (("antenna", ["--source-power-dbm", "-30"]),
                 ("attenuator", ["--source-power-dbm", "-30"]),
                 ("compression", []),
                 ("compression", ["--source-power-dbm", "-30", "--compression-profile", "antenna"]),
                 ("compression", ["--source-power-dbm", "-30", "--compression-opsat-dbm", "23"]),
                 ("compression", ["--source-power-dbm", "-30", "--compression-diagnostic"]))
        for case, extra in cases:
            arguments = ["runner", case, "unused.wsv", "unused-output", "--compression-two-tone", *extra]
            with self.subTest(case=case, extra=extra), patch.object(sys, "argv", arguments), \
                    patch.object(runner, "execute") as execute, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    runner.main()
                self.assertEqual(failure.exception.code, 2)
                execute.assert_not_called()

    def test_cancellation_warnings_are_strictly_diagnostic(self):
        root = Path(__file__).resolve().parents[2]
        fixture = root / "validation/systemvue-2023-shared-compression-captures.json"
        capture = next(c for c in json.loads(fixture.read_text()) if c["diagnostic"])
        with self.assertRaises(ValueError):
            runner.validate_capture(capture, "compression")
        runner.validate_capture(capture, "compression", allow_cancellation_warning=True)
        for suffix in (" extra failure", " (WARNING) Unknown warning"):
            changed = dict(capture, manager_errors=capture["manager_errors"] + suffix)
            with self.assertRaises(ValueError):
                runner.validate_capture(changed, "compression", allow_cancellation_warning=True)
        with tempfile.TemporaryDirectory() as directory:
            command = [sys.executable, "-c", "print(" + repr(json.dumps(capture)) + ")"]
            output = Path(directory) / "diagnostic"
            self.assertEqual(runner.execute(command, output, "compression", 10,
                                            cancellation_diagnostic=True), 0)
            status = json.loads((output / "status.json").read_text())
            self.assertEqual(status["state"], "captured_diagnostic")
            self.assertFalse(status["eligible_for_compatibility"])
            with self.assertRaises(ValueError):
                runner.execute([], Path(directory) / "bad", "compression", 10,
                               cancellation_diagnostic=True, compression_diagnostic=True)

    def test_shared_compression_flags_reject_wrong_modes(self):
        cases = [("compression", ["--compression-same-frequency"]),
                 ("compression", ["--compression-locked"]),
                 ("antenna", ["--compression-show-totals"]),
                 ("coherent", ["--compression-disable-noise"]),
                 ("compression", ["--compression-cancellation-diagnostic"])]
        for case, flags in cases:
            with self.subTest(case=case, flags=flags), patch.object(
                    sys, "argv", ["runner", case, "unused", "unused-output", *flags]):
                with contextlib.redirect_stderr(io.StringIO()), patch.object(runner, "execute") as execute:
                    with self.assertRaises(SystemExit):
                        runner.main()
                    execute.assert_not_called()

    def test_diagnostic_capture_preserves_warning_and_excludes_acceptance(self):
        root = Path(__file__).resolve().parents[2]
        fixture = root / "validation/systemvue-2023-single-saturation-diagnostic-captures.json"
        capture = json.loads(fixture.read_text())[0]
        with tempfile.TemporaryDirectory() as directory:
            for diagnostic in (False, True):
                output = Path(directory) / str(diagnostic)
                command = [sys.executable, "-c",
                           "import json,sys; from pathlib import Path; "
                           "print(json.dumps(json.loads(Path(sys.argv[1]).read_text())[0]))", str(fixture)]
                self.assertEqual(runner.execute(command, output, "compression", 10,
                                               compression_diagnostic=diagnostic), 0 if diagnostic else 1)
                status = json.loads((output / "status.json").read_text())
                self.assertEqual(status["state"], "captured_diagnostic" if diagnostic else "failed")
                self.assertEqual(json.loads((output / "capture.json").read_text()), capture)
                if diagnostic:
                    self.assertFalse(status["eligible_for_compatibility"])
            with self.assertRaises(ValueError):
                runner.execute([], Path(directory) / "wrong-case", "antenna", 10,
                               compression_diagnostic=True)

    def test_saturation_override_rejects_other_cases_and_profiles(self):
        for case, extra in (("antenna", []), ("attenuator", []),
                            ("compression", ["--compression-profile", "antenna"]),
                            ("compression", ["--compression-profile", "limiter"])):
            arguments = ["runner", case, "unused.wsv", "unused-output",
                         "--compression-opsat-dbm", "26", *extra]
            with self.subTest(case=case), patch.object(sys, "argv", arguments), \
                    patch.object(runner, "execute") as execute, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    runner.main()
                self.assertEqual(failure.exception.code, 2)
                execute.assert_not_called()

    def test_diagnostic_option_rejects_other_cases_and_profiles(self):
        for case, extra in (("antenna", []), ("attenuator", []),
                            ("compression", ["--compression-profile", "antenna"]),
                            ("compression", ["--compression-profile", "limiter"])):
            arguments = ["runner", case, "unused.wsv", "unused-output", "--compression-diagnostic", *extra]
            with self.subTest(case=case), patch.object(sys, "argv", arguments), \
                    patch.object(runner, "execute") as execute, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    runner.main()
                self.assertEqual(failure.exception.code, 2)
                execute.assert_not_called()

    def test_success_and_stale_rejection(self):
        root = Path(__file__).resolve().parents[2]
        sample = json.loads((root / "validation/systemvue-2023-attenuator-1db.json").read_text())
        capture = {key: sample[key] for key in
                   ("run_started_utc", "run_returned_utc", "manager_errors")}
        capture["nodes"] = [{"path": "RFModel_AttenuatorNoise/Designs/System1_Data_Folder/System1_Sch1_Data_Path1",
                             "timestamp": sample["dataset_timestamp"]}]
        with tempfile.TemporaryDirectory() as directory:
            for stale in (False, True):
                if stale:
                    capture["nodes"][0]["timestamp"] = "1"
                command = [sys.executable, "-c", "print(" + repr(json.dumps(capture)) + ")"]
                output = Path(directory) / str(stale)
                self.assertEqual(runner.execute(command, output, "attenuator", 10), int(stale))
                self.assertEqual(json.loads((output / "status.json").read_text())["state"],
                                 "failed" if stale else "captured")

    def test_process_failure_preserves_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            code = runner.execute([sys.executable, "-c", "import sys; print('failure', file=sys.stderr); sys.exit(3)"],
                                  output, "antenna", 10)
            self.assertEqual(code, 1)
            self.assertIn("failure", (output / "stderr.log").read_text())
            with self.assertRaises(FileExistsError):
                runner.execute([], output, "antenna", 10)

    def test_timeout_does_not_kill_or_retry(self):
        process = Mock(pid=123)
        process.wait.side_effect = subprocess.TimeoutExpired(["collector"], 1)
        with tempfile.TemporaryDirectory() as directory, patch.object(runner.subprocess, "Popen", return_value=process) as launch:
            output = Path(directory) / "run"
            self.assertEqual(runner.execute(["collector"], output, "antenna", 1), 124)
            launch.assert_called_once()
            process.kill.assert_not_called()
            process.terminate.assert_not_called()
            self.assertEqual(json.loads((output / "status.json").read_text())["state"], "timeout_unresolved")


if __name__ == "__main__":
    unittest.main()
