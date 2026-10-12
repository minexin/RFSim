#!/usr/bin/env python3
"""Verify a wheel against an installed native SDK without importing checkout code.

Run from any directory. The work directory must be new. Every tests/test_*.py
binding suite is required to support --installed and is run in a separate
isolated Python process. The JSON report records actual import locations and
native-library identity as well as unittest counts and failures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
import traceback
import zipfile


def fingerprint(path):
    path = Path(path).resolve(strict=True)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def installed_library(prefix, configuration, system=None):
    system = system or platform.system()
    suffix = 'd' if configuration == 'Debug' else ''
    names = {'Windows': f'rfmodel_c{suffix}.dll', 'Linux': f'librfmodel_c{suffix}.so',
             'Darwin': f'librfmodel_c{suffix}.dylib'}
    if system not in names:
        raise ValueError('Unsupported native-library platform: ' + system)
    prefix = Path(prefix).resolve(strict=True)
    candidates = list(prefix.rglob(names[system]))
    if len(candidates) != 1:
        raise ValueError(f'Expected exactly one installed {names[system]}, found {len(candidates)}')
    library = candidates[0].resolve(strict=True)
    if not library.is_file() or not library.is_relative_to(prefix):
        raise ValueError('Native library must resolve inside the install prefix')
    return library


def discover_suites(source):
    suites = sorted((source / 'tests').glob('test_*.py'))
    if not suites:
        raise ValueError('No Python binding suites found')
    for suite in suites:
        if '--installed' not in suite.read_text(encoding='utf-8-sig'):
            raise ValueError('Binding suite does not support installed mode: ' + suite.name)
    return suites


def validate_package(wheel, package_root, source):
    package_root = Path(package_root).resolve(strict=True)
    source_package = (source / 'python').resolve(strict=True)
    if package_root == source_package or package_root.is_relative_to(source_package):
        raise ValueError('Installed package root must not be the checkout Python tree')
    checked = []
    with zipfile.ZipFile(wheel) as archive:
        members = [name for name in archive.namelist() if name.startswith('rfmodel/') and name.endswith('.py')]
        metadata = [name for name in archive.namelist() if name.endswith('.dist-info/METADATA')]
        if 'rfmodel/__init__.py' not in members or len(metadata) != 1 or not metadata[0].startswith('rfmodel-'):
            raise ValueError('Expected one rfmodel distribution in wheel')
        if len(set(members)) != len(members):
            raise ValueError('Duplicate wheel package members')
        installed_members = {str(p.relative_to(package_root)).replace(os.sep, '/')
                             for p in (package_root / 'rfmodel').rglob('*.py')}
        if installed_members != set(members):
            raise ValueError('Installed Python module inventory differs from wheel')
        for member in members + metadata:
            relative = Path(member)
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Invalid wheel member path')
            target = (package_root / relative).resolve(strict=True)
            if not target.is_relative_to(package_root) or target.read_bytes() != archive.read(member):
                raise ValueError('Installed file differs from wheel: ' + member)
            if member in members:
                original = source_package / member
                if original.read_bytes() != archive.read(member):
                    raise ValueError('Wheel package differs from current source: ' + member)
                checked.append(dict(fingerprint(target), wheel_member=member,
                                    source_sha256=fingerprint(original)['sha256']))
    return checked


def clean_environment(package_root=None):
    # Remove inherited interpreter path/home and startup hooks. The one package
    # path is needed by CLI subprocesses created by the existing binding suites.
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith('PYTHON')}
    environment.update(PYTHONNOUSERSITE='1', PYTHONUTF8='1')
    if package_root is not None:
        environment['PYTHONPATH'] = str(package_root)
    return environment


def check_cli_environment(command, environment, package_root):
    if not isinstance(command, (list, tuple)) or not any(
        command[i:i+2] == ['-m', 'rfmodel'] or tuple(command[i:i+2]) == ('-m', 'rfmodel')
        for i in range(len(command)-1)
    ):
        return False
    paths = environment.get('PYTHONPATH', '').split(os.pathsep)
    if len(paths) != 1 or Path(paths[0]).resolve() != package_root.resolve():
        raise ValueError('RFModel CLI must use only the installed package PYTHONPATH')
    return True


def worker(arguments):
    import runpy
    import unittest
    parser = argparse.ArgumentParser()
    parser.add_argument('--package-root', required=True, type=Path)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--suite', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args(arguments)
    package_root = args.package_root.resolve(strict=True)
    suite = args.suite.resolve(strict=True)
    result = {'suite': str(suite), 'passed': False, 'unittest_runs': [], 'modules': [], 'cli_invocations': []}
    status = 1
    try:
        # -I excludes cwd, PYTHONPATH and user site from this worker. Explicitly
        # insert only the wheel installation before loading any RFModel module.
        sys.path.insert(0, str(package_root))
        import rfmodel
        if Path(rfmodel.__file__).resolve() != package_root / 'rfmodel' / '__init__.py':
            raise ValueError('RFModel was imported from outside the wheel installation')
        library = rfmodel.Library(args.library)
        result['library'] = fingerprint(library.path)
        result['abi_version'] = library._dll.rfmodel_abi_version()
        original_run = unittest.TextTestRunner.run
        original_popen = subprocess.Popen

        class InstalledPopen(original_popen):
            def __init__(self, args, *positional, **keywords):
                command = args
                environment = keywords.get('env')
                if environment is None:
                    environment = os.environ
                if check_cli_environment(command, environment, package_root):
                    result['cli_invocations'].append({
                        'command': list(command), 'python_path': environment['PYTHONPATH'],
                    })
                super().__init__(command, *positional, **keywords)


        def capture_run(runner, tests):
            observed = original_run(runner, tests)
            result['unittest_runs'].append({
                'tests_run': observed.testsRun, 'failures': len(observed.failures),
                'errors': len(observed.errors), 'skipped': len(observed.skipped),
                'expected_failures': len(observed.expectedFailures),
                'unexpected_successes': len(observed.unexpectedSuccesses),
                'successful': observed.wasSuccessful(),
            })
            return observed

        unittest.TextTestRunner.run = capture_run
        subprocess.Popen = InstalledPopen
        sys.argv = [str(suite), str(args.library.resolve(strict=True)), '--installed']
        try:
            runpy.run_path(str(suite), run_name='__main__')
            status = 0
        except SystemExit as error:
            status = 0 if error.code is None else error.code if isinstance(error.code, int) else 1
        finally:
            unittest.TextTestRunner.run = original_run
            subprocess.Popen = original_popen
        if not result['unittest_runs'] or sum(run['tests_run'] for run in result['unittest_runs']) == 0:
            raise ValueError('Suite did not execute any unittest cases')
        for name, module in sorted(sys.modules.items()):
            if name == 'rfmodel' or name.startswith('rfmodel.'):
                origin = Path(module.__file__).resolve(strict=True)
                if not origin.is_relative_to(package_root / 'rfmodel'):
                    raise ValueError('RFModel module imported from outside wheel: ' + name)
                result['modules'].append(dict(fingerprint(origin), name=name))
        result['passed'] = status == 0 and all(run['successful'] for run in result['unittest_runs'])
    except Exception as error:
        result['error'] = str(error)
        traceback.print_exc()
    write_json(args.report, result)
    return 0 if result['passed'] else 1


def verify(arguments):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--wheel', type=Path)
    group.add_argument('--wheel-dir', type=Path)
    parser.add_argument('--install-prefix', type=Path, required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--package-root', type=Path, help='Verify an existing wheel installation instead of invoking pip')
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--timeout', type=float, default=180, help='Timeout in seconds for each test or install process')
    args = parser.parse_args(arguments)
    if not 0 < args.timeout <= 3600:
        parser.error('timeout must be in (0, 3600] seconds')
    # A fresh directory prevents stale reports or package modules from being
    # mistaken for results of this run; never remove an existing directory.
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    report_path = work / 'installed-python-validation.json'
    report = {'schema_version': 1, 'passed': False, 'configuration': args.configuration,
              'platform': platform.platform(), 'python': sys.version, 'executable': sys.executable,
              'suites': [], 'total_tests': 0, 'skipped_tests': 0,
              'verifier': fingerprint(Path(__file__))}
    started = time.monotonic()
    try:
        source = args.source_root.resolve(strict=True)
        wheels = [args.wheel] if args.wheel else list(args.wheel_dir.glob('rfmodel-*.whl'))
        if len(wheels) != 1:
            raise ValueError(f'Expected exactly one RFModel wheel, found {len(wheels)}')
        wheel = wheels[0].resolve(strict=True)
        library = installed_library(args.install_prefix, args.configuration)
        suites = discover_suites(source)
        report.update(wheel=fingerprint(wheel), native_library=fingerprint(library),
                      source_root=str(source), suites_expected=[p.name for p in suites])
        package_root = args.package_root.resolve(strict=True) if args.package_root else work / 'site'
        if not args.package_root:
            command = [sys.executable, '-I', '-m', 'pip', 'install', '--no-index', '--no-deps',
                       '--disable-pip-version-check', '--target', str(package_root), str(wheel)]
            with (work / 'install.log').open('w', encoding='utf-8') as stream:
                installed = subprocess.run(command, cwd=work, env=clean_environment(),
                                           stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                                           timeout=args.timeout)
            report['installation'] = dict(command=command, returncode=installed.returncode,
                                          log=fingerprint(work / 'install.log'))
            if installed.returncode:
                raise RuntimeError('Wheel installation failed; see install.log')
        report['package_root'] = str(package_root)
        report['installed_modules'] = validate_package(wheel, package_root, source)
        environment = clean_environment(package_root)
        for suite in suites:
            directory = work / suite.stem
            directory.mkdir()
            result_path = directory / 'result.json'
            log_path = directory / 'test.log'
            command = [sys.executable, '-I', str(Path(__file__).resolve()), '_worker',
                       '--package-root', str(package_root), '--library', str(library),
                       '--suite', str(suite), '--report', str(result_path)]
            entry = {'suite': suite.name, 'source': fingerprint(suite), 'passed': False}
            begin = time.monotonic()
            try:
                with log_path.open('w', encoding='utf-8') as stream:
                    completed = subprocess.run(command, cwd=directory, env=environment,
                                               stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                                               timeout=args.timeout)
                entry['returncode'] = completed.returncode
                if result_path.exists():
                    observed = json.loads(result_path.read_text(encoding='utf-8'))
                    entry['worker'] = observed
                    report['total_tests'] += sum(run['tests_run'] for run in observed.get('unittest_runs', []))
                    report['skipped_tests'] += sum(run['skipped'] for run in observed.get('unittest_runs', []))
                    entry['passed'] = completed.returncode == 0 and observed['passed']
                else:
                    entry['error'] = 'Worker did not produce a report'
            except subprocess.TimeoutExpired:
                entry['error'] = 'Suite timed out'
            entry.update(seconds=time.monotonic() - begin, log=fingerprint(log_path))
            report['suites'].append(entry)
            print(f"{suite.name}: {'passed' if entry['passed'] else 'FAILED'}", flush=True)
        report['passed'] = len(report['suites']) == len(suites) and all(s['passed'] for s in report['suites'])
    except Exception as error:
        report['error'] = str(error)
        traceback.print_exc()
    report['seconds'] = time.monotonic() - started
    write_json(report_path, report)
    print(f"Installed wheel: {report['total_tests']} tests, {report['skipped_tests']} skipped; report {report_path}", flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '_worker':
        sys.exit(worker(sys.argv[2:]))
    sys.exit(verify(sys.argv[1:]))
