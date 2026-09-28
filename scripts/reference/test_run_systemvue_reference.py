import importlib.util
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
