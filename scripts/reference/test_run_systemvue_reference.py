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
    def test_two_tone_rejects_ambiguous_or_incompatible_options(self):
        cases = (("antenna", ["--source-power-dbm", "-30"]),
                 ("attenuator", ["--source-power-dbm", "-30"]),
                 ("compression", []),
                 ("compression", ["--source-power-dbm", "-30", "--compression-profile", "limiter"]),
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
