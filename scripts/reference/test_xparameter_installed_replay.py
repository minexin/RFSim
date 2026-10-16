"""Independent contracts for the developer-only installed X-parameter replay.

These are synthetic fixtures, never vendor inputs or a vendor replay pass.  The
expected typed values below are authored independently of the native probe.  No
Python code parses a candidate MDF/XNP file.  Set RFMODEL_XPARAMETER_PROBE to an
already-built native executable, and RFMODEL_XPARAMETER_SDK to an installed SDK,
to enable the explicitly skipped native and real-build integration tests.
RFMODEL_XPARAMETER_CMAKE (or CMAKE_COMMAND) can select an existing CMake binary.
"""

import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import xparameter_replay_score as score

_SPEC = importlib.util.spec_from_file_location(
    "installed_xparameter_replay", HERE / "replay-installed-xparameter-evidence.py")
replay = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(replay)

PROBE = os.environ.get("RFMODEL_XPARAMETER_PROBE")
SDK = os.environ.get("RFMODEL_XPARAMETER_SDK")
CMAKE = os.environ.get("RFMODEL_XPARAMETER_CMAKE",
                       os.environ.get("CMAKE_COMMAND", "cmake"))
FILE_IDS = ("ZFL_11AD_PHDModel.mdf", "X-param_BalAmp_ZFL11AD.mdf",
            "Multitone_Volterra_X-device.xnp", "AMP_Xparams.mdf")
RELATIVE_DIRECTORY = Path("Examples/RF Architecture Design/RF Design Kit/X Parameters")


LEGACY = '''! Autonomous synthetic contract, not installed vendor evidence.
VAR fund_1(real) = 2e9
VAR AN_outer(real) = 999
VAR bias(int) = -3
VAR note(string) = '轨迹 "外部"'
BEGIN PHDModelData
# z0(real) = 50
# AN_constant(real) = 1000
% AN_1_1(real) FI_3(real) FB_2_1(complex) S_2_1(complex)
% T_2_1(complex) FI_4(real) FB_2_9(complex) FB_1_1(complex) tag(string) AN_count(int)
5 0.25 1 -2 3 4
! Comments between physical groups must not consume a group.
-5 6 0.75 7 8 9 10 'unsampled "值"' 7
1 99 91 92 93 94
95 96 97 98 99 100 101 second 8
END
VAR fund_1(real) = 3e9
BEGIN PHDModelData
% AN_1_1(real) AN_2_1(real) FI_later(real)
9 -4 999
-2 8 998
END
'''.encode("utf-8")

ATTRIBUTES = b'''BEGIN XParamAttributes
% Index(int) Version(real)
% NumPorts(int) NumFundFreqs(int)
0 2.0
2 1
END
'''
PORTS = '''BEGIN XParamPortData
% PortNumber(int) RefZ0(complex)
% PortName(string)
1 50 -2
'输入 "甲"'
END
BEGIN XParamPortData
% PortNumber(int) RefZ0(complex)
% PortName(string)
2 75 0
"O'Brien output"
END
'''.encode("utf-8")
V2 = ATTRIBUTES + PORTS + LEGACY.replace(b"PHDModelData", b"XParamData")
# A third byte identity with the same independently declared native contract.
V2_DISTINCT = b"REM Distinct synthetic bytes only; no simulation.\n" + V2
FIXTURE_BYTES = (LEGACY, LEGACY, V2, V2_DISTINCT)


def record(name, kind, value):
    return {"name": name, "type": kind, "value": value}


def expected_native(version="legacy"):
    """Literal contract, not a projection of the program being tested."""
    result = {
        "schema_version": 1,
        "reader_version": version,
        "simulation_executed": False,
        "numerical_model_compatibility": False,
        "all_data_group_shapes_checked": True,
        "block_counts": {"PHDModelData": 2},
        "coefficient_rows_per_block": [2, 2],
        "coefficient_columns_per_row": [10, 3],
        "first_block_column_families": {"AN": 2, "FI": 2, "FB": 3,
                                          "S": 1, "T": 1, "tag": 1},
        "AN_raw_ranges": {"AN_1_1": {"min": -2.0, "max": 9.0, "first": 5.0},
                          "AN_2_1": {"min": -4.0, "max": 8.0, "first": -4.0}},
        "first_row_small_sample": [
            record("FI_3", "real", 0.25),
            record("FB_2_1", "complex", [1.0, -2.0]),
            record("S_2_1", "complex", [3.0, 4.0]),
            record("T_2_1", "complex", [-5.0, 6.0]),
            record("FI_4", "real", 0.75)],
        "metadata_blocks": {},
        "coefficient_block_outer_variables": [
            [record("fund_1", "real", 2e9), record("AN_outer", "real", 999.0),
             record("bias", "integer", -3), record("note", "string", '轨迹 "外部"')],
            [record("fund_1", "real", 3e9)]],
        "legacy_constants": [[record("z0", "real", 50.0),
                               record("AN_constant", "real", 1000.0)]],
    }
    if version == "2.0":
        result["block_counts"] = {"XParamAttributes": 1, "XParamPortData": 2,
                                  "XParamData": 2}
        result["metadata_blocks"] = {
            "XParamAttributes": [[record("Index", "integer", 0),
                                  record("Version", "real", 2.0),
                                  record("NumPorts", "integer", 2),
                                  record("NumFundFreqs", "integer", 1)]],
            "XParamPortData": [
                [record("PortNumber", "integer", 1), record("RefZ0", "complex", [50.0, -2.0]),
                 record("PortName", "string", '输入 "甲"')],
                [record("PortNumber", "integer", 2), record("RefZ0", "complex", [75.0, 0.0]),
                 record("PortName", "string", "O'Brien output")]],
        }
    return result


def synthetic_selected():
    selected = []
    for index, (file_id, data) in enumerate(zip(FILE_IDS, FIXTURE_BYTES)):
        summary = expected_native("legacy" if index < 2 else "2.0")
        summary.update(file_id=file_id, bytes=len(data),
                       sha256=hashlib.sha256(data).hexdigest())
        selected.append(summary)
    return selected


def descendants(value, prefix=()):
    yield prefix, value
    if isinstance(value, dict):
        for key, child in value.items():
            yield from descendants(child, prefix + (key,))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from descendants(child, prefix + (index,))


def replace_at(value, path, replacement):
    parent = value
    for key in path[:-1]:
        parent = parent[key]
    parent[path[-1]] = replacement


def manifest(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


class StrictJsonAndScoringTests(unittest.TestCase):
    def test_single_strict_json_document(self):
        data = ' {"字符串": [1, 2.5, true, null]} \n'.encode("utf-8")
        self.assertEqual(score.strict_json(data), {"字符串": [1, 2.5, True, None]})
        for bad in (b'{"a":1,"a":2}', b'{"a":{"b":1,"b":1}}', b'{}{}',
                    b'{}\nextra stdout', b'prefix {}', b'NaN', b'Infinity',
                    b'-Infinity', b'{"x":1e9999}', b'{"x":"\xff"}',
                    b'{"x":"\\ud800"}', b'{"\\udfff":0}', b'\xef\xbb\xbf{}',
                    b'{"a":', b''):
            with self.subTest(bad=bad), self.assertRaises(score.ReplayError):
                score.strict_json(bad)
        for bad in ("{}", bytearray(b"{}"), None):
            with self.subTest(kind=type(bad)), self.assertRaises(score.ReplayError):
                score.strict_json(bad)
        with self.assertRaises(score.ReplayError):
            score.strict_json(b"{}", limit=1)
        self.assertEqual(score.strict_json(b"{}", limit=2), {})
        with self.assertRaises(score.ReplayError):
            score.strict_json(b"[" * 2000 + b"0" + b"]" * 2000)

    def test_synthetic_exact_match_has_four_files_three_hashes_no_simulations(self):
        expected = synthetic_selected()
        result = score.score_selected(copy.deepcopy(expected), expected)
        self.assertEqual(result, {"selected_files": 4, "unique_content_hashes": 3,
            "duplicate_content_groups": [[FILE_IDS[0], FILE_IDS[1]]],
            "independent_simulations": 0})
        self.assertEqual(tuple(score.FILE_IDS), FILE_IDS)
        self.assertEqual(score.RELATIVE_DIRECTORY, RELATIVE_DIRECTORY)
        self.assertEqual(set(expected_native()), set(score.NATIVE_KEYS))

    def test_every_leaf_mutation_is_rejected(self):
        expected = synthetic_selected()
        count = 0
        for path, original in descendants(expected):
            if isinstance(original, (dict, list)):
                continue
            changed = (not original if type(original) is bool else
                       original + 1 if type(original) in (float, int) else
                       original + "-changed")
            actual = copy.deepcopy(expected)
            replace_at(actual, path, changed)
            with self.subTest(path=path), self.assertRaises(score.ReplayError):
                score.score_selected(actual, expected)
            count += 1
        self.assertGreater(count, 250)

    def test_every_dictionary_rejects_added_and_missing_keys(self):
        expected = synthetic_selected()
        for path, original in descendants(expected):
            if not isinstance(original, dict):
                continue
            variants = [dict(original, unexpected_key=0)]
            variants.extend({k: v for k, v in original.items() if k != removed}
                            for removed in original)
            for changed in variants:
                actual = copy.deepcopy(expected)
                replace_at(actual, path, changed)
                with self.subTest(path=path, keys=tuple(changed)), self.assertRaises(score.ReplayError):
                    score.score_selected(actual, expected)

    def test_every_list_rejects_length_or_meaningful_order_changes(self):
        expected = synthetic_selected()
        for path, original in descendants(expected):
            if not isinstance(original, list):
                continue
            variants = [original + [None]]
            if original:
                variants.append(original[:-1])
            if original[::-1] != original:
                variants.append(original[::-1])
            for changed in variants:
                actual = copy.deepcopy(expected)
                if path:
                    replace_at(actual, path, changed)
                else:
                    actual = changed
                with self.subTest(path=path), self.assertRaises(score.ReplayError):
                    score.score_selected(actual, expected)

    def test_numeric_bool_and_structural_types_are_not_interchangeable(self):
        expected = synthetic_selected()
        for path, original in descendants(expected):
            if type(original) not in (bool, int, float):
                continue
            changed = (int(original) if type(original) is bool else
                       float(original) if type(original) is int else True)
            actual = copy.deepcopy(expected)
            replace_at(actual, path, changed)
            with self.subTest(path=path), self.assertRaises(score.ReplayError):
                score.score_selected(actual, expected)
        for wanted, actual in ((1.0, True), (False, 0), (1, 1.0),
                               ({}, []), ([], {}), ("1", 1)):
            with self.subTest(wanted=wanted, actual=actual), self.assertRaises(score.ReplayError):
                score.exact(actual, wanted)
        score.exact(1, 1.0)  # A real typed value can round-trip as a JSON integer.
        for invalid in (float("inf"), float("-inf"), float("nan")):
            with self.assertRaises(score.ReplayError):
                score.exact(invalid, 1.0)
        for actual, expected in ((-0.0, 0.0), (0.0, -0.0), (10 ** 400, 1.0)):
            with self.subTest(actual=actual, expected=expected), self.assertRaises(score.ReplayError):
                score.exact(actual, expected)

    def test_identity_is_checked_before_value_comparison(self):
        expected = synthetic_selected()
        actual = copy.deepcopy(expected)
        actual[0]["first_row_small_sample"][0]["value"] = 123.0
        actual[0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(score.ReplayError, r"\.sha256"):
            score.score_selected(actual, expected)
        actual[0]["bytes"] += 1
        with self.assertRaisesRegex(score.ReplayError, r"\.bytes"):
            score.score_selected(actual, expected)
        for actual in (None, {}, tuple(expected), [None] * 4,
                       expected[::-1], expected[:3], expected + expected[:1]):
            with self.subTest(actual_type=type(actual)), self.assertRaises(score.ReplayError):
                score.score_selected(actual, expected)

    def test_frozen_baseline_is_hash_pinned_and_only_a_projection(self):
        baseline, expected = score.load_baseline(replay.BASELINE)
        self.assertEqual(baseline["systemvue_version"], "2023.0.0.11903")
        self.assertEqual([item["file_id"] for item in expected], list(FILE_IDS))
        self.assertEqual(len({item["sha256"] for item in expected}), 3)
        self.assertTrue(all(item["simulation_executed"] is False for item in expected))
        with tempfile.TemporaryDirectory() as folder:
            altered = Path(folder) / "altered.json"
            altered.write_bytes(replay.BASELINE.read_bytes() + b"\n")
            with self.assertRaisesRegex(score.ReplayError, "baseline SHA256 mismatch"):
                score.load_baseline(altered)
        for bad in ("VAR x(real) = nan", "VAR x(real) = 1e9999",
                    "VAR x(string) = unrecognized", "# x(real) = 1", "VAR x(real) = 1 junk"):
            with self.subTest(bad=bad), self.assertRaises(score.ReplayError):
                score.declaration(bad, "VAR")


class FileIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="replay identity spaces ")
        self.addCleanup(self.temp.cleanup)
        # collect() passes a canonical installation root; the Windows temp path
        # may otherwise contain an 8.3 alias while read_once resolves its input.
        self.root = Path(self.temp.name).resolve()
        self.path = self.root / "synthetic input.mdf"
        self.path.write_bytes(LEGACY)

    def test_reads_exactly_once_and_preserves_bytes_and_identity(self):
        before = self.path.stat()
        original_open = Path.open
        calls = []

        def tracked(path, *args, **kwargs):
            calls.append((path, args, kwargs))
            return original_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", tracked):
            data, identity = replay.read_once(self.path, self.root)
        self.assertEqual(calls, [(self.path, ("rb",), {})])
        self.assertEqual(data, LEGACY)
        self.assertEqual(identity, replay.identity(before))
        self.assertEqual(replay.identity(self.path.stat()), identity)
        self.assertEqual(self.path.read_bytes(), LEGACY)

    def test_read_after_real_metadata_change_keeps_same_api_snapshots_stable(self):
        # A write-once fixture may have equal creation/change times on Windows,
        # hiding the stat(path)/fstat(fd) ctime difference. Change metadata first.
        os.utime(self.path, ns=(1234567890000000000, 1234567890000000000))
        before = replay.identity(self.path.stat())
        data, observed = replay.read_once(self.path, self.root)
        self.assertEqual(data, LEGACY)
        self.assertEqual(observed, before)
        self.assertEqual(replay.identity(self.path.stat()), before)

    def check_identity_snapshots(self, platform, path_before, fd_before, fd_after,
                                 path_after, error=None):
        """Exercise read_once without changing the host's global os.name."""
        opened = mock.MagicMock()
        opened.__enter__.return_value = opened
        opened.read.return_value = LEGACY
        module_os = SimpleNamespace(name=platform, fstat=mock.Mock())
        with mock.patch.object(Path, "open", return_value=opened) as open_file, \
                mock.patch.object(replay, "os", module_os), \
                mock.patch.object(replay, "identity", side_effect=[
                    path_before, fd_before, fd_after, path_after]) as identity:
            if error:
                with self.assertRaisesRegex(score.ReplayError, error):
                    replay.read_once(self.path, self.root)
            else:
                self.assertEqual(replay.read_once(self.path, self.root), (LEGACY, path_before))
        open_file.assert_called_once_with("rb")
        if error == "changed_before_read":
            opened.read.assert_not_called()
            self.assertEqual(module_os.fstat.call_count, 1)
            self.assertEqual(identity.call_count, 2)
        else:
            opened.read.assert_called_once_with(replay.MAX_INPUT_BYTES + 1)
            self.assertEqual(module_os.fstat.call_count, 2)
            self.assertEqual(identity.call_count, 4)

    def test_windows_stable_cross_api_ctime_difference_is_accepted(self):
        before = replay.identity(self.path.stat())
        descriptor = dict(before, ctime_ns=before["ctime_ns"] + 1)
        self.check_identity_snapshots("nt", before, descriptor, descriptor, before)

    def test_same_api_ctime_changes_are_rejected_on_every_platform(self):
        before = replay.identity(self.path.stat())
        for platform in ("nt", "posix"):
            descriptor = dict(before, ctime_ns=before["ctime_ns"] + (platform == "nt"))
            changed_path = dict(before, ctime_ns=before["ctime_ns"] + 2)
            changed_fd = dict(descriptor, ctime_ns=descriptor["ctime_ns"] + 2)
            for owner, after_fd, after_path in (("path", descriptor, changed_path),
                                               ("descriptor", changed_fd, before)):
                with self.subTest(platform=platform, changed=owner):
                    self.check_identity_snapshots(platform, before, descriptor, after_fd,
                                                  after_path, "changed_during_read")

    def test_cross_api_non_ctime_identity_changes_are_rejected_before_read(self):
        before = replay.identity(self.path.stat())
        for platform in ("nt", "posix"):
            for key in ("device", "inode", "size", "mtime_ns", "mode"):
                descriptor = dict(before)
                descriptor[key] += 1
                with self.subTest(platform=platform, changed=key):
                    self.check_identity_snapshots(platform, before, descriptor, descriptor,
                                                  before, "changed_before_read")

    def test_nonwindows_cross_api_ctime_mismatch_still_fails_before_read(self):
        before = replay.identity(self.path.stat())
        descriptor = dict(before, ctime_ns=before["ctime_ns"] + 1)
        self.check_identity_snapshots("posix", before, descriptor, descriptor, before,
                                      "changed_before_read")

    def test_descriptor_identity_mutation_fails_without_retry(self):
        before = self.path.stat()
        changed = SimpleNamespace(**{name: getattr(before, name) for name in
            ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode")})
        changed.st_mtime_ns += 1
        for values, error in (([changed], "changed_before_read"),
                              ([before, changed], "changed_during_read")):
            with self.subTest(error=error), mock.patch.object(
                    replay.os, "fstat", side_effect=values) as fstat:
                with self.assertRaisesRegex(score.ReplayError, error):
                    replay.read_once(self.path, self.root)
                self.assertEqual(fstat.call_count, len(values))
        self.assertEqual(self.path.read_bytes(), LEGACY)

    def test_path_identity_mutation_and_truncation_are_rejected(self):
        real_identity = replay.identity(self.path.stat())
        changed = dict(real_identity, inode=real_identity["inode"] + 1)
        with mock.patch.object(replay, "identity", side_effect=
                [real_identity, real_identity, real_identity, changed]) as identity:
            with self.assertRaisesRegex(score.ReplayError, "changed_during_read"):
                replay.read_once(self.path, self.root)
            self.assertEqual(identity.call_count, 4)
        opened = mock.MagicMock()
        opened.__enter__.return_value = opened
        opened.read.return_value = LEGACY[:-1]
        with mock.patch.object(Path, "open", return_value=opened) as open_file, \
                mock.patch.object(replay.os, "fstat", return_value=self.path.stat()):
            with self.assertRaisesRegex(score.ReplayError, "changed_during_read"):
                replay.read_once(self.path, self.root)
        open_file.assert_called_once_with("rb")
        opened.read.assert_called_once_with(replay.MAX_INPUT_BYTES + 1)

    def test_symlink_outside_installation_is_rejected(self):
        directory = self.root / "installation"
        directory.mkdir()
        link = directory / "selected.mdf"
        try:
            link.symlink_to(self.path)
        except (OSError, NotImplementedError) as error:
            self.skipTest("This platform cannot create a test symlink: " + str(error))
        with self.assertRaisesRegex(score.ReplayError, "outside_installation"):
            replay.read_once(link, directory)
        self.assertEqual(self.path.read_bytes(), LEGACY)

    def test_nonregular_and_oversized_files_are_rejected_before_open(self):
        with self.assertRaisesRegex(score.ReplayError, "type_or_size"):
            replay.read_once(self.root, self.root)
        with mock.patch.object(replay, "MAX_INPUT_BYTES", len(LEGACY) - 1):
            with self.assertRaisesRegex(score.ReplayError, "type_or_size"):
                replay.read_once(self.path, self.root)


class CommandCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="replay command spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.commands = []

    def invoke(self, program, name="probe", **kwargs):
        return replay.run_command([sys.executable, "-c", program], self.root, name,
                                  self.commands, **kwargs)

    def command_record(self, name="probe"):
        return score.strict_json((self.root / (name + ".command.json")).read_bytes())

    def test_success_preserves_stdout_stderr_and_one_invocation(self):
        output = self.invoke("import sys; data=sys.stdin.buffer.read(); "
                             "sys.stdout.buffer.write(data); sys.stderr.write('diagnostic')",
                             stdin=b"binary\r\n\x1a\x00\xff bytes")
        self.assertEqual(output, b"binary\r\n\x1a\x00\xff bytes")
        self.assertEqual((self.root / "probe.stdout").read_bytes(), output)
        self.assertEqual((self.root / "probe.stderr").read_bytes(), b"diagnostic")
        record = self.command_record()
        self.assertEqual(record["returncode"], 0)
        self.assertFalse(record["output_limit_exceeded"])
        self.assertEqual(record["stdin_bytes"], len(output))
        self.assertEqual(self.commands, [record])

    def test_nonzero_retains_failure_artifacts_without_retry(self):
        counter = self.root / "invocation count"
        script = ("from pathlib import Path; import sys; "
                  f"p=Path({str(counter)!r}); p.write_text(p.read_text()+'x' if p.exists() else 'x'); "
                  "sys.stdout.buffer.write(b'partial output\\r\\n'); sys.stdout.buffer.flush(); "
                  "sys.stderr.write('failure'); sys.exit(7)")
        with self.assertRaisesRegex(score.ReplayError, "command_failed_probe"):
            self.invoke(script)
        self.assertEqual(counter.read_text(), "x")
        self.assertEqual(len(self.commands), 1)
        self.assertEqual(self.command_record()["returncode"], 7)
        self.assertEqual((self.root / "probe.stdout").read_bytes(), b"partial output\r\n")
        self.assertEqual((self.root / "probe.stderr").read_bytes(), b"failure")

    def test_timeout_retains_failure_without_retry(self):
        # Interpreter startup can exceed the short deadline on a busy host.
        # Prepare one real process and wait for its flushed output before testing
        # the runner's unchanged timeout/kill/capture path. Do not consume pipes.
        ready = self.root / "timeout child ready"
        script = ("from pathlib import Path; import sys, time; "
                  "sys.stdout.buffer.write(b'started\\n'); sys.stdout.buffer.flush(); "
                  f"Path({str(ready)!r}).write_bytes(b'ready'); time.sleep(30)")
        argv = [sys.executable, "-c", script]
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, shell=False)

        def cleanup_process():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for pipe in (process.stdout, process.stderr):
                pipe.close()

        self.addCleanup(cleanup_process)
        startup_deadline = time.monotonic() + 10
        while not ready.is_file():
            self.assertIsNone(process.poll(), "Timeout fixture exited before readiness")
            self.assertLess(time.monotonic(), startup_deadline,
                            "Timeout fixture did not become ready during setup")
            time.sleep(0.01)
        started = time.monotonic()
        with mock.patch.object(replay.subprocess, "Popen", return_value=process) as launch:
            with self.assertRaisesRegex(score.ReplayError, "command_failed_probe"):
                replay.run_command(argv, self.root, "probe", self.commands, timeout=0.2)
        self.assertLess(time.monotonic() - started, 10)
        launch.assert_called_once_with(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, env=None, shell=False)
        self.assertTrue(self.command_record()["timed_out"])
        self.assertNotEqual(self.command_record()["returncode"], 0)
        self.assertIsNotNone(process.poll())
        self.assertEqual(len(self.commands), 1)
        self.assertEqual((self.root / "probe.stdout").read_bytes(), b"started\n")

    def test_stdout_and_stderr_are_bounded_during_capture(self):
        for stream in ("stdout", "stderr"):
            with self.subTest(stream=stream), self.assertRaises(score.ReplayError):
                self.invoke(f"import sys; sys.{stream}.buffer.write(b'x'*262144); "
                            f"sys.{stream}.flush()", name=stream,
                            stdout_limit=128, stderr_limit=128)
            self.assertEqual(len((self.root / (stream + "." + stream)).read_bytes()), 128)
            self.assertTrue(self.command_record(stream)["output_limit_exceeded"])
        self.assertEqual(len(self.commands), 2)

    def test_start_failure_retains_empty_logs_and_one_attempt(self):
        with self.assertRaisesRegex(replay.Blocked, "tool_start_failed_missing"):
            replay.run_command([str(self.root / "no executable")], self.root,
                               "missing", self.commands)
        self.assertEqual(len(self.commands), 1)
        self.assertEqual(self.command_record("missing")["start_error"], "FileNotFoundError")
        self.assertEqual((self.root / "missing.stdout").read_bytes(), b"")
        self.assertEqual((self.root / "missing.stderr").read_bytes(), b"")


class SyntheticWrapperUnitTests(unittest.TestCase):
    """Mocked toolchain and baseline only here; never evidence of vendor replay."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="synthetic wrapper with spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.installation = self.root / "fake installation"
        selected = self.installation / RELATIVE_DIRECTORY
        selected.mkdir(parents=True)
        for name, data in zip(FILE_IDS, FIXTURE_BYTES):
            (selected / name).write_bytes(data)
        self.sdk = self.root / "fake installed SDK"
        self.header = self.sdk / "include/rfmodel/io/xparameter_data.hpp"
        self.header.parent.mkdir(parents=True)
        self.header.write_bytes(b"synthetic header placeholder, never compiled\n")
        config = self.sdk / "lib/cmake/RFModel"
        config.mkdir(parents=True)
        for name in ("RFModelConfig.cmake", "RFModelConfigVersion.cmake", "RFModelTargets.cmake"):
            (config / name).write_bytes(b"synthetic config, never compiled\n")
        self.args = argparse.Namespace(installation_root=str(self.installation),
            sdk_prefix=str(self.sdk), output_dir=str(self.root / "exclusive output"),
            cmake=sys.executable, compiler=None, generator=None, config="Release", timeout=1.0)
        self.native_calls = []
        self.failure = None
        self.raw_override = None

    def fake_command(self, argv, output, name, commands, stdin=None, **kwargs):
        commands.append({"argv": list(map(str, argv)), "test_name": name})
        if name == "configure":
            build = output / "build"
            build.mkdir()
            for filename, text in (("compiler-path.txt", sys.executable),
                    ("compiler-id.txt", "synthetic test"), ("compiler-version.txt", "0"),
                    ("reader-header.txt", str(self.header))):
                (build / filename).write_text(text)
        elif name == "build":
            binary = output / "build/bin"
            binary.mkdir()
            (binary / ("xparameter_replay_probe.exe" if os.name == "nt" else
                       "xparameter_replay_probe")).write_bytes(b"synthetic, never executed")
        elif name.startswith("file-"):
            index = int(name.split("-")[1]) - 1
            self.native_calls.append((name, list(argv), stdin, kwargs))
            if name == self.failure:
                raise score.ReplayError("synthetic_command_failure")
            if self.raw_override is not None:
                return self.raw_override
            return json.dumps(expected_native("legacy" if index < 2 else "2.0"),
                              ensure_ascii=False).encode("utf-8")
        return b""

    def collect(self):
        # This is the only baseline substitution: an explicitly isolated unit
        # suite.  The actual-installed-SDK integration below never patches it.
        with mock.patch.object(replay, "load_baseline", return_value=(
                {"systemvue_version": "synthetic unit test only"}, synthetic_selected())), \
                mock.patch.object(replay, "run_command", side_effect=self.fake_command), \
                mock.patch.dict(os.environ, {"CXX": "", "CL": "", "_CL_": ""}):
            return replay.collect(self.args)

    def test_exclusive_output_refusal_happens_before_any_artifact_write(self):
        output = Path(self.args.output_dir)
        output.mkdir()
        (output / "keep me").write_bytes(b"untouched")
        before = manifest(self.root)
        with mock.patch.object(replay, "load_baseline") as baseline:
            with self.assertRaises(FileExistsError):
                replay.collect(self.args)
        baseline.assert_not_called()
        self.assertEqual(before, manifest(self.root))

    def test_synthetic_pipeline_uses_exact_bytes_once_and_retains_inputs(self):
        inputs = manifest(self.installation)
        sdk = manifest(self.sdk)
        report = self.collect()
        self.assertEqual(report["status"], "selected_installed_file_replay_pass")
        self.assertEqual(report["baseline_systemvue_version"], "synthetic unit test only")
        self.assertEqual(report["unique_content_hashes"], 3)
        self.assertEqual(report["independent_simulations"], 0)
        self.assertFalse(report["simulation_executed"])
        self.assertFalse(report["numerical_model_compatibility"])
        self.assertEqual(report["observed_systemvue_version"], "unknown")
        self.assertEqual(len(report["commands"]), 7)
        self.assertEqual([call[0] for call in self.native_calls], ["file-1", "file-2", "file-3", "file-4"])
        for index, (_, argv, data, kwargs) in enumerate(self.native_calls):
            self.assertEqual(argv[1:], ["--json", "--stdin"])
            self.assertEqual(data, FIXTURE_BYTES[index])
            self.assertEqual(kwargs["stdout_limit"], score.MAX_SUMMARY_BYTES)
            self.assertEqual(kwargs["timeout"], self.args.timeout)
        self.assertEqual(manifest(self.installation), inputs)
        self.assertEqual(manifest(self.sdk), sdk)
        self.assertEqual(score.strict_json((Path(self.args.output_dir) / "replay.json").read_bytes()), report)

    def test_one_file_failure_is_retained_and_remaining_files_run_once(self):
        self.failure = "file-2"
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["error"], "one_or_more_selected_files_failed")
        self.assertEqual(len(self.native_calls), 4)
        self.assertEqual(report["files"][1]["error"], "synthetic_command_failure")
        self.assertNotIn("summary", report["files"][1])
        self.assertTrue((Path(self.args.output_dir) / "replay.json").is_file())
        self.assertTrue((Path(self.args.output_dir) / "file-4.summary.json").is_file())
        self.assertNotIn("selected_files", report)

    def test_malformed_native_stdout_never_reaches_scoring(self):
        for suffix, raw in (("extra", b'{}\nextra stdout'),
                            ("duplicate", b'{"schema_version":1,"schema_version":1}'),
                            ("nonfinite", b'{"value":NaN}'), ("schema", b'{}')):
            with self.subTest(case=suffix):
                self.args.output_dir = str(self.root / suffix)
                self.raw_override = raw
                with mock.patch.object(replay, "score_selected") as scorer:
                    report = self.collect()
                scorer.assert_not_called()
                self.assertEqual(report["status"], "failed")
                self.assertEqual(len(report["files"]), 4)
                self.assertTrue(all("summary" not in item for item in report["files"]))

    def test_utf8_enrichment_cannot_exceed_summary_byte_limit(self):
        oversized = expected_native("2.0")
        oversized["metadata_blocks"]["XParamPortData"][0][2]["value"] = "界" * 50000
        self.raw_override = json.dumps(oversized, ensure_ascii=False).encode("utf-8")
        self.assertLess(len(self.raw_override), score.MAX_SUMMARY_BYTES)
        with mock.patch.object(replay, "score_selected") as scorer:
            report = self.collect()
        scorer.assert_not_called()
        self.assertEqual(report["status"], "failed")
        self.assertTrue(all(item["error"] == "enriched_summary_byte_limit"
                            for item in report["files"]))
        self.assertFalse(any(Path(self.args.output_dir).glob("file-*.summary.json")))

    def test_output_inside_input_root_is_refused_before_mkdir(self):
        before = manifest(self.root)
        for input_root in (self.installation, self.sdk):
            self.args.output_dir = str(input_root / "forbidden output")
            with self.subTest(input_root=input_root), self.assertRaisesRegex(
                    score.ReplayError, "output_must_be_outside_input_roots"):
                self.collect()
            self.assertFalse(Path(self.args.output_dir).exists())
        self.assertEqual(self.native_calls, [])
        self.assertEqual(manifest(self.root), before)


@unittest.skipUnless(PROBE, "Native probe tests not run: set RFMODEL_XPARAMETER_PROBE to an already-built probe")
class NativeProbeContractTests(unittest.TestCase):
    def run_probe(self, data, *args):
        return subprocess.run([PROBE, "--json", "--stdin", *args], input=data,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)

    def test_legacy_multiline_groups_ranges_and_first_five_first_block(self):
        result = self.run_probe(LEGACY)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stderr, b"")
        actual = score.strict_json(result.stdout)
        score.exact(actual, expected_native())
        self.assertNotIn("AN_outer", actual["AN_raw_ranges"])
        self.assertNotIn("AN_constant", actual["AN_raw_ranges"])
        self.assertNotIn("AN_count", actual["AN_raw_ranges"])
        self.assertNotEqual(actual["AN_raw_ranges"]["AN_1_1"]["first"],
                            actual["AN_raw_ranges"]["AN_1_1"]["min"])
        self.assertEqual([x["name"] for x in actual["first_row_small_sample"]],
                          ["FI_3", "FB_2_1", "S_2_1", "T_2_1", "FI_4"])

    def test_v2_metadata_group_order_utf8_and_quotes(self):
        result = self.run_probe(V2)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        actual = score.strict_json(result.stdout)
        score.exact(actual, expected_native("2.0"))
        self.assertEqual(actual["metadata_blocks"]["XParamPortData"][0][2]["value"], '输入 "甲"')
        self.assertEqual(actual["metadata_blocks"]["XParamPortData"][1][2]["value"], "O'Brien output")

    def test_bom_crlf_file_path_with_spaces_and_text_mode(self):
        data = b"\xef\xbb\xbf" + LEGACY.replace(b"\n", b"\r\n")
        with tempfile.TemporaryDirectory(prefix="native path spaces ") as folder:
            path = Path(folder) / "synthetic input.mdf"
            path.write_bytes(data)
            result = subprocess.run([PROBE, "--json", str(path)], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            score.exact(score.strict_json(result.stdout), expected_native())
            text = subprocess.run([PROBE, str(path)], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(text.returncode, 0, text.stderr)
            self.assertIn(b"version=legacy", text.stdout)
            self.assertIn(b"numerical model compatibility is NOT established", text.stdout)
            self.assertEqual(path.read_bytes(), data)

    def test_invalid_data_and_late_error_emit_no_partial_stdout(self):
        for data in (b"", b"BEGIN PHDModelData\n% x(real)\nNaN\nEND\n",
                b"BEGIN PHDModelData\n% x(real)\n% y(real)\n1\nEND\n",
                b"BEGIN PHDModelData\n% x(complex)\n1\nEND\n",
                b"BEGIN PHDModelData\n% x(real) x(real)\n1 2\nEND\n",
                b"BEGIN XParamData\n% x(real)\n1\nEND\n",
                ATTRIBUTES, LEGACY + b"VAR dangling(real) = 1\n"):
            with self.subTest(data=data[:60]):
                result = self.run_probe(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertTrue(result.stderr)
                self.assertLess(len(result.stderr), 256)

    def test_invalid_utf8_even_in_unsampled_values_emits_no_stdout(self):
        for invalid in (b"\xff", b"\xc0\xaf", b"\xe2\x82", b"\xed\xa0\x80",
                        b"\xf4\x90\x80\x80"):
            for data in (V2.replace('输入'.encode(), invalid),
                         LEGACY.replace(b"second", b"hidden" + invalid),
                         LEGACY.replace('轨迹'.encode(), invalid)):
                with self.subTest(invalid=invalid, data=data[:24]):
                    result = self.run_probe(data)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b"")
                    self.assertIn(b"encoding_error", result.stderr)
                    self.assertLess(len(result.stderr), 256)

    def test_parser_and_summary_limits_emit_no_partial_stdout(self):
        scalar = b"BEGIN PHDModelData\n% x(real)\n1\nEND\n"
        limits = {
            "field-name": b"BEGIN PHDModelData\n% " + b"x" * 257 + b"(real)\n1\nEND\n",
            "string-value": LEGACY.replace(b"second", b"x" * (16 * 1024 + 1)),
            "physical-line": b"!" + b"x" * (1024 * 1024) + b"\n" + scalar,
            "block-count": scalar * 4097,
            "record-count": ATTRIBUTES + b"BEGIN XParamPortData\n% PortNumber(int)\n" +
                b"1\n" * 4097 + b"END\n" + b"BEGIN XParamData\n% x(real)\n1\nEND\n",
            "summary-bytes": ATTRIBUTES + b"BEGIN XParamPortData\n% PortName(string)\n" +
                (b"x" * 10000 + b"\n") * 30 + b"END\n" +
                b"BEGIN XParamData\n% x(real)\n1\nEND\n",
        }
        for case, data in limits.items():
            with self.subTest(case=case):
                result = self.run_probe(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"resource_limit", result.stderr)
                self.assertLess(len(result.stderr), 256)

    def test_native_total_input_byte_limit(self):
        # Each comment is independently below the line limit.  This crosses
        # the 64 MiB total-input limit without hitting a different limit first.
        data = (b"!" + b"x" * 65534 + b"\n") * 1024 + b"!\n"
        self.assertEqual(len(data), 64 * 1024 * 1024 + 2)
        result = self.run_probe(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"resource_limit", result.stderr)
        self.assertLess(len(result.stderr), 256)

    def test_invalid_cli_never_emits_stdout(self):
        for args in ([], ["--json"], ["--json", "--json", "--stdin"],
                     ["--stdin", "--stdin"], ["--unknown"], ["--stdin", "file.mdf"]):
            with self.subTest(args=args):
                result = subprocess.run([PROBE, *args], input=LEGACY, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, timeout=30)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"usage:", result.stderr)


@unittest.skipUnless(SDK, "Installed-SDK integration not run: set RFMODEL_XPARAMETER_SDK to an existing installed SDK")
class ActualInstalledSdkIntegrationTests(unittest.TestCase):
    def test_real_build_four_synthetic_files_fail_vendor_identity_gate(self):
        """Actual installed-header build, unmocked baseline, no vendor pass claim."""
        with tempfile.TemporaryDirectory(prefix="installed replay spaces ") as folder:
            root = Path(folder)
            installation = root / "synthetic installation with spaces"
            selected = installation / RELATIVE_DIRECTORY
            selected.mkdir(parents=True)
            for name, data in zip(FILE_IDS, FIXTURE_BYTES):
                (selected / name).write_bytes(data)
            before = manifest(installation)
            output = root / "exclusive evidence with spaces"
            command = [sys.executable, str(HERE / "replay-installed-xparameter-evidence.py"),
                "--installation-root", str(installation), "--sdk-prefix", SDK,
                "--output-dir", str(output), "--cmake", CMAKE]
            for variable, option in (("RFMODEL_XPARAMETER_COMPILER", "--compiler"),
                                     ("RFMODEL_XPARAMETER_CONFIG", "--config"),
                                     ("RFMODEL_XPARAMETER_GENERATOR", "--generator")):
                if os.environ.get(variable):
                    command.extend([option, os.environ[variable]])
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=240)
            self.assertEqual(result.returncode, 1, result.stderr.decode(errors="replace"))
            report = score.strict_json((output / "replay.json").read_bytes(), limit=1024 * 1024)
            self.assertEqual(report["status"], "failed", report)
            self.assertRegex(report["error"], r"mismatch at .*\.(bytes|sha256)$")
            self.assertEqual(report["baseline_sha256"], score.BASELINE_SHA256)
            self.assertEqual(report["observed_systemvue_version"], "unknown")
            self.assertFalse(report["simulation_executed"])
            self.assertFalse(report["numerical_model_compatibility"])
            self.assertEqual(report["independent_simulations"], 0)
            self.assertEqual([item["file_id"] for item in report["files"]], list(FILE_IDS))
            self.assertEqual(len({item["sha256"] for item in report["files"]}), 3)
            self.assertEqual(len(report["commands"]), 7)
            probes = [entry for entry in report["commands"] if "--stdin" in entry["argv"]]
            self.assertEqual(len(probes), 4)
            self.assertEqual([entry["stdin_bytes"] for entry in probes], list(map(len, FIXTURE_BYTES)))
            self.assertTrue(all(entry["returncode"] == 0 for entry in probes))
            for index, item in enumerate(report["files"]):
                self.assertEqual(item["status"], "parsed_not_yet_scored")
                score.exact(item["summary"], synthetic_selected()[index])
                self.assertTrue((output / f"file-{index + 1}.stdout").is_file())
                self.assertTrue((output / f"file-{index + 1}.command.json").is_file())
            self.assertEqual(manifest(installation), before)
            preserved = manifest(output)
            again = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(again.returncode, 2)
            self.assertEqual(again.stdout, b"")
            self.assertIn(b"Replay refused", again.stderr)
            self.assertEqual(manifest(output), preserved)
            self.assertEqual(manifest(installation), before)


if __name__ == "__main__":
    unittest.main()
