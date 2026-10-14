"""Offline mutated archival fixtures only. These are not vendor captures."""
import copy
import contextlib
import io
from types import SimpleNamespace
import importlib.util
import json
import math
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
spec = importlib.util.spec_from_file_location('pilot', Path(__file__).with_name('rfamp-parameter-pilot.py'))
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)
archive = pilot.load('archive-cascade-reference')
comparison = archive.comparison
runner = comparison.runner


class PilotTests(unittest.TestCase):
    def fixture(self, index=1):
        captures = json.loads((pilot.ROOT / 'validation/systemvue-2023-eleventh-order-captures.json').read_text())
        raw = copy.deepcopy(captures[index])
        references = json.loads((pilot.ROOT / 'validation/systemvue-2023-highorder-input-references.json').read_text())
        ref = next(r for r in references if r['raw_capture_sha256'] == raw['raw_capture_sha256'])
        names = dict(zip(comparison.vector(raw, 'IDNo'), comparison.vector(raw, 'IDName')))
        vectors = {k: [] for k in ('F1', 'P1', 'V1', 'Z1', 'ID1')}
        for source in ref['sources']:
            identifier = next(n for n, label in names.items() if label.startswith('{') and label.endswith(source['label']))
            for row in source['samples']:
                for key, field in (('F1', 'frequency_hz'), ('P1', 'power_w')):
                    vectors[key].append(row[field])
                vectors['ID1'].append(identifier)
                vectors['V1'].extend(row['peak_voltage'])
                vectors['Z1'].extend(row['impedance_ohms'])
        for key, values in vectors.items():
            raw['nodes'].append(dict(path=comparison.ROOT + comparison.SPECTRUM + key,
                                     data=values, dimensions=[len(values)]))
        return raw

    def status(self, raw):
        command = ['powershell.exe', '-WorkspacePath', 'D:/RFModel/build-reference/RFModel_CascadeIntermods.wsv', '-DisableSpectrumReduction']
        for flag, key in (('-SourcePowerDbm', 'source_power_dbm'), ('-SourcePhaseDeg', 'source_phase_deg'),
                          ('-MaximumOrder', 'maximum_order'), ('-SecondGainDb', 'second_gain_db'),
                          ('-ChannelBandwidthHz', 'channel_bandwidth_hz'), ('-SecondaryRangeDb', 'secondary_range_db'),
                          ('-ReverseIsolationDb', 'reverse_isolation_db'), ('-FirstOp1dbDbm', 'first_op1db_dbm'),
                          ('-SecondSourcePowerDbm', 'second_source_power_dbm')):
            if key in raw:
                command += [flag, str(raw[key])]
        if raw['two_tone']:
            command += ['-TwoTone']
        return dict(case='cascade', state='captured', collector_exit_code=0,
                    eligible_for_compatibility=True, command=command)

    def test_default_and_new_readbacks_and_old_scorer_boundary(self):
        raw = self.fixture()
        comparison.inspect(raw)
        archive.extract(raw, self.status(raw), raw['raw_capture_sha256'])
        raw['first_op1db_dbm'] = 21
        raw['submitted_script'] = raw['submitted_script'].replace('RFAmp1.ParamSet.OP1dB.Set("20")', 'RFAmp1.ParamSet.OP1dB.Set("21")')
        with self.assertRaisesRegex(ValueError, 'Parameter mismatch'):
            archive.extract(raw, self.status(raw), raw['raw_capture_sha256'])
        comparison.node(raw, 'Design3/PartList/RFAmp1/ParamSet/OP1dB')['data'] = runner.op1db_watts(21)
        archive.extract(raw, self.status(raw), raw['raw_capture_sha256'])
        with self.assertRaisesRegex(ValueError, 'old coefficients'):
            comparison.inspect(raw)
        comparison.node(raw, 'Design3/PartList/RFAmp2/ParamSet/OP1dB')['data'] = runner.op1db_watts(21)
        with self.assertRaisesRegex(ValueError, 'Parameter mismatch'):
            archive.extract(raw, self.status(raw), raw['raw_capture_sha256'])

    def test_invalid_and_negative_dbm(self):
        for value in (math.nan, math.inf, -math.inf, True, '21', 1e6, -1e6):
            with self.subTest(value=value), self.assertRaises(ValueError):
                runner.op1db_watts(value)
        self.assertGreater(runner.op1db_watts(-100), 0)
        self.assertEqual(runner.op1db_watts(20), 0.1)

    def test_command_binding(self):
        raw = self.fixture()
        raw['first_op1db_dbm'] = 21
        raw['submitted_script'] = raw['submitted_script'].replace('RFAmp1.ParamSet.OP1dB.Set("20")', 'RFAmp1.ParamSet.OP1dB.Set("21")')
        comparison.node(raw, 'Design3/PartList/RFAmp1/ParamSet/OP1dB')['data'] = runner.op1db_watts(21)
        status = self.status(raw)
        status['command'][status['command'].index('-FirstOp1dbDbm') + 1] = '20'
        with self.assertRaisesRegex(ValueError, 'Command disagrees'):
            archive.extract(raw, status, raw['raw_capture_sha256'])

    def test_frozen_plan_survives_real_git_autocrlf_checkout(self):
        expected = pilot.PLAN.read_bytes()
        self.assertEqual(pilot.digest(expected), pilot.PLAN_SHA256)
        attributes = (pilot.ROOT / '.gitattributes').read_bytes()
        rule = b'/validation/rfamp-op1db-pilot-plan.json -text'
        for protected in (False, True):
            with self.subTest(protected=protected), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                plan = root / 'validation/rfamp-op1db-pilot-plan.json'
                plan.parent.mkdir()
                plan.write_bytes(expected)
                selected = attributes if protected else b''.join(
                    line for line in attributes.splitlines(keepends=True) if line.strip() != rule)
                (root / '.gitattributes').write_bytes(selected)
                control = root / 'newline-control.txt'
                control.write_bytes(b'first line\nsecond line\n')
                def git(*args):
                    return subprocess.run(['git', '-c', 'core.autocrlf=true', '-C', str(root), *args],
                                          capture_output=True, check=True, timeout=30)
                git('init', '--quiet')
                git('add', '.gitattributes', 'validation/rfamp-op1db-pilot-plan.json', 'newline-control.txt')
                # Force a genuine working-tree materialization, not an index up-to-date shortcut.
                plan.unlink()
                control.unlink()
                git('checkout-index', '--all', '--force')
                self.assertEqual(control.read_bytes(), b'first line\r\nsecond line\r\n')
                with patch.object(pilot, 'PLAN', plan):
                    if not protected:
                        self.assertNotEqual(plan.read_bytes(), expected)
                        self.assertEqual(plan.read_bytes(), expected.replace(b'\n', b'\r\n'))
                        with self.assertRaisesRegex(ValueError, 'Frozen pilot plan changed'):
                            pilot.frozen_plan()
                        continue
                    self.assertEqual(plan.read_bytes(), expected)
                    self.assertEqual(pilot.digest(plan.read_bytes()), pilot.PLAN_SHA256)
                    pilot.frozen_plan()
                    semantic = expected.replace(b'"first_op1db_dbm": 20', b'"first_op1db_dbm": 22', 1)
                    self.assertNotEqual(json.loads(semantic), json.loads(expected))
                    for mutation in (expected.replace(b'\n', b'\r\n'), semantic):
                        plan.write_bytes(mutation)
                        with self.assertRaisesRegex(ValueError, 'Frozen pilot plan changed'):
                            pilot.frozen_plan()

    def test_attribute_only_upgrade_requires_scoped_plan_restore(self):
        expected = pilot.PLAN.read_bytes()
        attributes = (pilot.ROOT / '.gitattributes').read_bytes()
        rule = b'/validation/rfamp-op1db-pilot-plan.json -text'
        old_attributes = b''.join(line for line in attributes.splitlines(keepends=True)
                                  if line.strip() != rule)
        relative_plan = 'validation/rfamp-op1db-pilot-plan.json'
        stale_sha256 = '050ade7d5032c5a14a4ef699c9df9950d8f8500798ac46e32306bce6aa184f18'
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            plan = root / relative_plan
            plan.parent.mkdir()
            plan.write_bytes(expected)
            attribute_file = root / '.gitattributes'
            attribute_file.write_bytes(old_attributes)
            control = root / 'unrelated.txt'
            control.write_bytes(b'original\n')
            def git(*args):
                return subprocess.run(['git', '-c', 'core.autocrlf=true',
                                      '-c', 'user.name=Offline Fixture', '-c', 'user.email=fixture@example.invalid',
                                      '-c', 'commit.gpgsign=false', '-C', str(root), *args],
                                      capture_output=True, check=True, timeout=30).stdout
            git('init', '--quiet')
            git('add', '.')
            git('commit', '--quiet', '-m', 'Unprotected plan')
            old = git('rev-parse', 'HEAD').decode().strip()
            attribute_file.write_bytes(attributes)
            git('add', '.gitattributes')
            git('commit', '--quiet', '-m', 'Protect unchanged plan blob')
            new = git('rev-parse', 'HEAD').decode().strip()
            self.assertEqual(git('rev-parse', old + ':' + relative_plan),
                             git('rev-parse', new + ':' + relative_plan))
            git('checkout', '--quiet', '--detach', old)
            plan.unlink()
            git('checkout-index', '--all', '--force')
            self.assertEqual(pilot.digest(plan.read_bytes()), stale_sha256)
            git('checkout', '--quiet', '--detach', new)
            # An attribute-only upgrade does not rematerialize the unchanged blob.
            self.assertEqual(pilot.digest(plan.read_bytes()), stale_sha256)
            self.assertTrue(git('diff', '--', relative_plan))
            with patch.object(pilot, 'PLAN', plan):
                with self.assertRaisesRegex(ValueError, 'Frozen pilot plan changed'):
                    pilot.frozen_plan()
                backup = root / 'preserved-plan-copy.json'
                backup.write_bytes(plan.read_bytes())
                control.write_bytes(b'intentional unrelated edits\n')
                git('restore', '--source=HEAD', '--worktree', '--', relative_plan)
                self.assertEqual(plan.read_bytes(), expected)
                pilot.frozen_plan()
                self.assertEqual(pilot.digest(backup.read_bytes()), stale_sha256)
                self.assertEqual(control.read_bytes(), b'intentional unrelated edits\n')
                self.assertFalse(git('diff', '--', relative_plan))

    @unittest.skipUnless(sys.platform == 'win32', 'PowerShell AST parsing requires Windows; no SystemVue invoked')
    def test_powershell_syntax_only(self):
        with tempfile.TemporaryDirectory() as temp:
            prepared = Path(temp) / 'prepared'
            pilot.prepare(prepared, 'D:/RF Model', 'D:/pilot runs')
            files = [pilot.HERE / 'capture-cascade-reference.ps1', *prepared.glob('*.ps1')]
            for path in files:
                quoted = "'" + str(path).replace("'", "''") + "'"
                script = ('$tokens=$null; $errors=$null; '
                          '[System.Management.Automation.Language.Parser]::ParseFile('
                          + quoted + ', [ref]$tokens, [ref]$errors) | Out-Null; '
                          'if ($errors.Count -gt 0) { $errors | ForEach-Object { '
                          '[Console]::Error.WriteLine($_.ToString()) }; exit 1 }; exit 0')
                result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
                                        capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(result.returncode, 0, str(path) + ': ' + result.stdout + result.stderr)

    def test_wrapper_generates_override_without_changing_defaults(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            script = root / 'scripts/reference/run-systemvue-reference.py'
            script.parent.mkdir(parents=True)
            script.touch()
            workspace = root / 'build-reference/RFModel_CascadeIntermods.wsv'
            workspace.parent.mkdir()
            workspace.touch()
            for override in (None, 21, -100):
                args = ['runner', 'cascade', str(workspace), str(root / 'out'), '--source-power-dbm', '5']
                if override is not None:
                    args += ['--cascade-first-op1db-dbm', str(override)]
                with patch.object(runner, '__file__', str(script)), patch.object(runner, 'os', SimpleNamespace(name='nt')), patch.object(sys, 'argv', args), patch.object(runner, 'execute', return_value=0) as execute:
                    self.assertEqual(runner.main(), 0)
                    command = execute.call_args.args[0]
                    self.assertEqual('-FirstOp1dbDbm' in command, override is not None)
                    if override is not None:
                        self.assertEqual(float(command[command.index('-FirstOp1dbDbm') + 1]), override)
                    self.assertEqual(command[command.index('-SecondGainDb') + 1], '10')
            for case, value in (('cascade', 'nan'), ('cascade', 'inf'), ('cascade', '-1000000'), ('compression', '21')):
                args = ['runner', case, 'unused', 'unused', '--source-power-dbm', '5', '--cascade-first-op1db-dbm=' + value]
                with patch.object(sys, 'argv', args), patch.object(runner, 'execute') as execute, contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        runner.main()
                    execute.assert_not_called()

    def test_execute_rejects_missing_mismatched_duplicate_readback(self):
        for failure in ('missing_metadata', 'mismatched_readback', 'duplicate_option', 'valid'):
            raw = self.fixture()
            raw['first_op1db_dbm'] = 21
            raw['submitted_script'] = raw['submitted_script'].replace('RFAmp1.ParamSet.OP1dB.Set("20")', 'RFAmp1.ParamSet.OP1dB.Set("21")')
            comparison.node(raw, 'Design3/PartList/RFAmp1/ParamSet/OP1dB')['data'] = runner.op1db_watts(21)
            command = ['collector', '-FirstOp1dbDbm', '21']
            if failure == 'missing_metadata':
                del raw['first_op1db_dbm']
            elif failure == 'mismatched_readback':
                comparison.node(raw, 'Design3/PartList/RFAmp1/ParamSet/OP1dB')['data'] = 0.1
            elif failure == 'duplicate_option':
                command += ['-FirstOp1dbDbm', '21']
            def launch(command, stdout, stderr):
                stdout.write(json.dumps(raw).encode())
                return Mock(pid=123, wait=Mock(return_value=0))
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp, patch.object(runner.subprocess, 'Popen', side_effect=launch):
                output = Path(temp) / 'run'
                self.assertEqual(runner.execute(command, output, 'cascade', 10), 0 if failure == 'valid' else 1)
                status = json.loads((output / 'status.json').read_text())
                self.assertEqual(status['state'], 'captured' if failure == 'valid' else 'failed')
                self.assertTrue((output / 'capture.json').exists())

    def test_frozen_commands_and_generation(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'prepared'
            manifest = pilot.prepare(output, 'D:/RF Model', 'D:/pilot runs')
            self.assertEqual(len(manifest['runs']), 4)
            self.assertEqual(manifest['plan']['analysis_budget'], 4)
            for run, (_, op1db, two, phase) in zip(manifest['runs'], pilot.RUNS):
                command = run['command']
                self.assertEqual(command[command.index('--cascade-first-op1db-dbm') + 1], str(op1db))
                self.assertEqual(command[command.index('--cascade-phase-deg') + 1], str(phase))
                self.assertEqual('--cascade-two-tone' in command, two)
                script = (output / (run['run_id'] + '.ps1')).read_text()
                self.assertIn('Get-FileHash -LiteralPath', script)
                self.assertIn("'D:\\RF Model\\scripts\\reference\\run-systemvue-reference.py'", script)
            with self.assertRaises(FileExistsError):
                pilot.prepare(output, 'D:/RF Model', 'D:/pilot runs')
        collector = (pilot.HERE / 'capture-cascade-reference.ps1').read_text()
        self.assertIn('[double]$FirstOp1dbDbm = 20', collector)
        self.assertIn('i == 1 ? firstOp1db.ToString("R", CultureInfo.InvariantCulture) : "20"', collector)
        self.assertIn('first_op1db_dbm = $FirstOp1dbDbm', collector)
        with self.assertRaises(ValueError):
            pilot.commands('relative', 'D:/runs')

    def build_runs(self, temp):
        preparation, runs = Path(temp) / 'prepared', Path(temp) / 'runs'
        manifest = pilot.prepare(preparation, 'D:/RFModel', 'D:/runs')
        for i, (name, op1db, two, phase) in enumerate(pilot.RUNS):
            raw = self.fixture(3 if two else 1)
            raw.update(first_op1db_dbm=op1db, second_source_power_dbm=5)
            raw['submitted_script'] = raw['submitted_script'].replace('RFAmp1.ParamSet.OP1dB.Set("20")', f'RFAmp1.ParamSet.OP1dB.Set("{op1db}")')
            comparison.node(raw, 'Design3/PartList/RFAmp1/ParamSet/OP1dB')['data'] = runner.op1db_watts(op1db)
            # Mutated fixture timestamps deliberately synthetic, never delivered as vendor evidence.
            raw['run_started_utc'] = f'2026-10-14T12:0{i}:00Z'
            raw['run_returned_utc'] = f'2026-10-14T12:0{i}:01Z'
            for suffix in ('System3_Design3_Data', 'System3_Data_Folder/System3_Design3_Data_Path1'):
                comparison.node(raw, suffix)['timestamp'] = str(1791979200 + 60 * i)
            status = self.status(raw)
            status['command'][:1] = ['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'RemoteSigned', '-File', 'D:/RFModel/scripts/reference/capture-cascade-reference.ps1']
            if not two:
                iarg = status['command'].index('-SecondSourcePowerDbm')
                del status['command'][iarg:iarg+2]
            status['submitted_script_sha256'] = pilot.digest(raw['submitted_script'].encode())
            status['source_sha256'] = {k: manifest['source_sha256'][v] for k, v in (('collector.ps1', pilot.SOURCES[0]), ('runner.py', pilot.SOURCES[1]))}
            directory = runs / name
            directory.mkdir(parents=True)
            (directory / 'capture.json').write_text(json.dumps(raw))
            (directory / 'status.json').write_text(json.dumps(status))
            (directory / 'stderr.log').write_text('synthetic offline fixture, not vendor evidence\n')
            for k, v in (('collector.ps1', pilot.SOURCES[0]), ('runner.py', pilot.SOURCES[1])):
                (directory / k).write_bytes((pilot.HERE / v).read_bytes())
        return preparation, runs

    def test_packet_preserves_complete_arrays_and_original_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            preparation, runs = self.build_runs(temp)
            # Include an additional non-source port-1 row: the source-only archive
            # must not be mistaken for preservation of the complete raw vectors.
            raw_path = runs / 'A-baseline/capture.json'
            extra = json.loads(raw_path.read_text())
            pairs = zip(comparison.vector(extra, 'IDNo'), comparison.vector(extra, 'IDName'))
            other_id = next(n for n, label in pairs if label.startswith('{1}') and ',RFAmp' in label)
            for key in ('F1', 'P1', 'V1', 'Z1', 'ID1'):
                node = comparison.node(extra, comparison.SPECTRUM + key)
                node['data'] += [other_id] if key == 'ID1' else node['data'][:2 if key in ('V1', 'Z1') else 1]
                node['dimensions'] = [len(node['data'])]
            raw_path.write_text(json.dumps(extra))
            output = Path(temp) / 'packet'
            packet = pilot.archive_packet(preparation, runs, output)
            self.assertEqual(len(packet['runs']), 4)
            for record in packet['runs']:
                source = runs / record['run_id']
                raw = json.loads((source / 'capture.json').read_text())
                for key in ('F1', 'P1', 'V1', 'Z1', 'ID1'):
                    self.assertEqual(comparison.node(record['capture'], comparison.SPECTRUM + key), comparison.node(raw, comparison.SPECTRUM + key))
                self.assertEqual((output / record['run_id'] / 'capture.json').read_bytes(), (source / 'capture.json').read_bytes())
                self.assertEqual(record['input_reference']['raw_capture_sha256'], record['capture']['raw_capture_sha256'])
                self.assertEqual(len(record['input_reference']['sources']), 2 if raw['two_tone'] else 1)
            self.assertIn('V1_peak/sqrt(2*50', packet['input_wave_convention'])
            with self.assertRaises(FileExistsError):
                pilot.archive_packet(preparation, runs, output)

    def test_packet_failclosed(self):
        for mutation in ('missing_port', 'wrong_profile', 'source_hash', 'script_hash', 'version', 'failed_status', 'missing_stderr', 'identity', 'manifest_command', 'collector_path', 'workspace_path', 'script_request', 'duplicate_script_request'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                preparation, runs = self.build_runs(temp)
                directory = runs / 'B-op1db21'
                raw = json.loads((directory / 'capture.json').read_text())
                status = json.loads((directory / 'status.json').read_text())
                if mutation == 'missing_port':
                    raw['nodes'] = [n for n in raw['nodes'] if not n['path'].endswith('/V1')]
                elif mutation == 'wrong_profile':
                    raw['first_op1db_dbm'] = 20
                elif mutation == 'source_hash':
                    (directory / 'collector.ps1').write_text('changed')
                elif mutation in ('script_request', 'duplicate_script_request'):
                    if mutation == 'script_request':
                        raw['submitted_script'] = raw['submitted_script'].replace('RFAmp1.ParamSet.OP1dB.Set("21")', 'RFAmp1.ParamSet.OP1dB.Set("20")')
                    else:
                        raw['submitted_script'] += 'Call wsdoc.Designs.Design3.PartList.RFAmp1.ParamSet.OP1dB.Set("21")\r\n'
                    status['submitted_script_sha256'] = pilot.digest(raw['submitted_script'].encode())
                elif mutation == 'script_hash':
                    status['submitted_script_sha256'] = '0' * 64
                elif mutation == 'version':
                    raw['systemvue_version'] = 'wrong'
                elif mutation == 'failed_status':
                    status['state'] = 'timeout_unresolved'
                elif mutation == 'missing_stderr':
                    (directory / 'stderr.log').unlink()
                elif mutation == 'collector_path':
                    status['command'][status['command'].index('-File') + 1] = 'D:/untrusted.ps1'
                elif mutation == 'workspace_path':
                    status['command'][status['command'].index('-WorkspacePath') + 1] = 'E:/RFModel/build-reference/RFModel_CascadeIntermods.wsv'
                else:
                    m = json.loads((preparation / 'manifest.json').read_text())
                    if mutation == 'identity':
                        m['runs'].reverse()
                    else:
                        m['runs'][0]['command'][1] = 'D:/changed.py'
                    (preparation / 'manifest.json').write_text(json.dumps(m))
                (directory / 'capture.json').write_text(json.dumps(raw))
                (directory / 'status.json').write_text(json.dumps(status))
                output = Path(temp) / 'packet'
                with self.assertRaises((ValueError, FileNotFoundError)):
                    pilot.archive_packet(preparation, runs, output)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
