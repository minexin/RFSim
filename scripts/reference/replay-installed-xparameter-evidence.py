#!/usr/bin/env python3
"""Read-only, developer-only installed-file replay using the installed native SDK.

No SystemVue execution, vendor parser in Python, full-file artifact, installation,
source-tree include fallback, retries, or numerical compatibility inference.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import threading

from xparameter_replay_score import (BASELINE_SHA256, FILE_IDS, RELATIVE_DIRECTORY,
    MAX_SUMMARY_BYTES, NATIVE_KEYS, ReplayError, load_baseline, score_selected,
    sha256, strict_json)

MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_LOG_BYTES = 1024 * 1024
HERE = Path(__file__).resolve().parent
BASELINE = HERE.parent.parent / 'validation/systemvue-xparameter-format-evidence-20261015.json'

class Blocked(ReplayError):
    pass

def now():
    return datetime.now(timezone.utc).isoformat()

def save(path, data):
    with Path(path).open('xb') as stream:
        stream.write(data)

def save_json(path, obj):
    save(path, (json.dumps(obj, ensure_ascii=True, allow_nan=False, indent=2) + '\n').encode())

def file_hash(path):
    import hashlib
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()

def inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False

def identity(st):
    return {'device': st.st_dev, 'inode': st.st_ino, 'size': st.st_size,
            'mtime_ns': st.st_mtime_ns, 'ctime_ns': st.st_ctime_ns,
            'mode': st.st_mode}

def read_once(path, root):
    """One bounded open/read, with descriptor and path identity checks; no retries."""
    resolved = path.resolve(strict=True)
    if not inside(resolved, root):
        raise ReplayError('selected_file_outside_installation')
    before = identity(path.stat())
    if not stat.S_ISREG(before['mode']) or before['size'] > MAX_INPUT_BYTES:
        raise ReplayError('selected_file_type_or_size')
    with path.open('rb') as stream:
        opened = identity(os.fstat(stream.fileno()))
        if opened != before:
            raise ReplayError('selected_file_changed_before_read')
        data = stream.read(MAX_INPUT_BYTES + 1)
        after_fd = identity(os.fstat(stream.fileno()))
    after = identity(path.stat())
    if (before != after_fd or before != after or len(data) != before['size'] or
            len(data) > MAX_INPUT_BYTES or path.resolve(strict=True) != resolved):
        raise ReplayError('selected_file_changed_during_read')
    return data, before

def run_command(argv, output, name, commands, stdin=None, timeout=120,
                stdout_limit=MAX_LOG_BYTES, stderr_limit=MAX_LOG_BYTES, env=None):
    """Bound both pipe captures during execution; preserve failed output once."""
    entry = {'argv': [str(x) for x in argv], 'started_utc': now(),
             'timeout_seconds': timeout, 'stdin_bytes': len(stdin) if stdin is not None else 0}
    commands.append(entry)
    captures = [bytearray(), bytearray()]
    exceeded = threading.Event()
    errors = []
    proc = None
    try:
        proc = subprocess.Popen(entry['argv'], stdin=subprocess.PIPE if stdin is not None
            else subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, shell=False)
        def drain(pipe, index, limit):
            try:
                while True:
                    chunk = pipe.read(8192)
                    if not chunk:
                        break
                    remaining = limit - len(captures[index])
                    captures[index].extend(chunk[:max(0, remaining)])
                    if len(chunk) > remaining:
                        exceeded.set()
                        proc.kill()
            except (OSError, ValueError) as exc:
                errors.append(type(exc).__name__)
            finally:
                pipe.close()
        workers = [threading.Thread(target=drain, args=(proc.stdout, 0, stdout_limit), daemon=True),
                   threading.Thread(target=drain, args=(proc.stderr, 1, stderr_limit), daemon=True)]
        def feed():
            try:
                proc.stdin.write(stdin)
                proc.stdin.close()
            except (BrokenPipeError, OSError):
                try:
                    proc.stdin.close()
                except OSError:
                    pass
        if stdin is not None:
            workers.append(threading.Thread(target=feed, daemon=True))
        for worker in workers:
            worker.start()
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            entry['timed_out'] = True
            proc.kill()
            proc.wait()
        for worker in workers:
            worker.join(timeout=5)
        if any(worker.is_alive() for worker in workers):
            # A child retaining inherited pipes is not a successful invocation.
            entry['pipe_incomplete'] = True
        entry['returncode'] = proc.returncode
        entry['output_limit_exceeded'] = exceeded.is_set()
        entry['pipe_errors'] = errors
    except OSError as exc:
        entry['start_error'] = type(exc).__name__
    finally:
        entry['ended_utc'] = now()
        save(output / (name + '.stdout'), bytes(captures[0]))
        save(output / (name + '.stderr'), bytes(captures[1]))
        save_json(output / (name + '.command.json'), entry)
    if entry.get('start_error'):
        raise Blocked('tool_start_failed_' + name)
    if (entry.get('timed_out') or entry.get('pipe_incomplete') or
            entry['output_limit_exceeded'] or errors or entry['returncode'] != 0):
        raise ReplayError('command_failed_' + name)
    return bytes(captures[0])

def collect(args):
    # Existing output is deliberately rejected before writing any artifact.
    output = Path(args.output_dir).expanduser().resolve()
    for input_root in (args.installation_root, args.sdk_prefix):
        if inside(output, Path(input_root).expanduser().resolve()):
            raise ReplayError('output_must_be_outside_input_roots')
    output.mkdir(parents=False, exist_ok=False)
    output = output.resolve(strict=True)
    report = {'schema_version': 1,
        'classification': 'installed_vendor_native_reader_replay_no_simulation',
        'status': 'failed', 'started_utc': now(), 'simulation_executed': False,
        'numerical_model_compatibility': False, 'independent_simulations': 0,
        'observed_systemvue_version': 'unknown', 'commands': [], 'files': []}
    try:
        installation = Path(args.installation_root).expanduser().resolve(strict=True)
        sdk = Path(args.sdk_prefix).expanduser().resolve(strict=True)
        if not installation.is_dir() or not sdk.is_dir():
            raise Blocked('installation_or_sdk_not_directory')
        if inside(output, installation) or inside(output, sdk):
            raise ReplayError('output_must_be_outside_input_roots')
        report.update(installation_root=str(installation), sdk_prefix=str(sdk),
                      configuration=args.config)
        baseline, expected = load_baseline(BASELINE)
        report.update(baseline_sha256=BASELINE_SHA256,
            baseline_systemvue_version=baseline['systemvue_version'])
        sources = output / 'sources'
        sources.mkdir()
        source_files = {'probe-xparameter-data.cpp': HERE / 'probe-xparameter-data.cpp',
            'odr.cpp': HERE / 'installed-xparameter-probe/odr.cpp',
            'CMakeLists.txt': HERE / 'installed-xparameter-probe/CMakeLists.txt',
            Path(__file__).name: Path(__file__),
            'xparameter_replay_score.py': HERE / 'xparameter_replay_score.py'}
        report['source_sha256'] = {}
        for name, path in source_files.items():
            data = path.read_bytes()
            save(sources / name, data)
            report['source_sha256'][name] = sha256(data)
        baseline_copy = BASELINE.read_bytes()
        if sha256(baseline_copy) != BASELINE_SHA256:
            raise ReplayError('baseline_changed_before_copy')
        save(sources / BASELINE.name, baseline_copy)
        cmake = shutil.which(args.cmake)
        if not cmake:
            raise Blocked('cmake_missing')
        cmake = str(Path(cmake).resolve(strict=True))
        report['python'] = {'version': sys.version, 'executable': sys.executable,
                            'sha256': file_hash(sys.executable)}
        report['cmake'] = {'executable': cmake, 'sha256': file_hash(cmake)}
        run_command([cmake, '--version'], output, 'cmake-version', report['commands'])
        header = sdk / 'include/rfmodel/io/xparameter_data.hpp'
        config = sdk / 'lib/cmake/RFModel'
        sdk_files = [header] + [config / name for name in ('RFModelConfig.cmake',
            'RFModelConfigVersion.cmake', 'RFModelTargets.cmake')]
        for path in sdk_files:
            if not path.is_file() or not inside(path.resolve(), sdk):
                raise Blocked('installed_sdk_missing_or_outside_prefix')
        before_sdk = {str(x): file_hash(x) for x in sdk_files}
        report['sdk_files_sha256'] = before_sdk
        report['sdk_package_version'] = '0.1.0'
        build = output / 'build'
        configure = [cmake, '-S', str(sources), '-B', str(build), '-DSDK_PREFIX=' + str(sdk),
            '-DCMAKE_BUILD_TYPE=' + args.config, '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']
        compiler_choice = args.compiler or os.environ.get('CXX')
        if os.environ.get('CL') or os.environ.get('_CL_'):
            raise Blocked('compiler_environment_arguments_unsupported')
        if compiler_choice:
            compiler = shutil.which(compiler_choice)
            if not compiler:
                raise Blocked('compiler_missing_or_launcher_arguments_unsupported')
            configure.append('-DCMAKE_CXX_COMPILER=' + str(Path(compiler).resolve()))
        if args.generator:
            configure += ['-G', args.generator]
        # Compiler-injected include environments could bypass installed-only checks.
        env = dict(os.environ)
        for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
                    'CXX', 'CL', '_CL_', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CMAKE_PREFIX_PATH', 'RFModel_DIR'):
            env.pop(key, None)
        try:
            run_command(configure, output, 'configure', report['commands'], timeout=180, env=env)
        except ReplayError as exc:
            raise Blocked('installed_sdk_configure_unavailable') from exc
        compiler_path = Path((build / 'compiler-path.txt').read_text())
        report['compiler'] = {'executable': str(compiler_path), 'sha256': file_hash(compiler_path),
            'id': (build / 'compiler-id.txt').read_text(),
            'version': (build / 'compiler-version.txt').read_text()}
        if Path((build / 'reader-header.txt').read_text()).resolve() != header.resolve():
            raise ReplayError('unexpected_reader_header')
        report['reader_header'] = {'path': str(header.resolve()), 'sha256': file_hash(header)}
        run_command([cmake, '--build', str(build), '--config', args.config, '--parallel', '2'],
                    output, 'build', report['commands'], timeout=180, env=env)
        if before_sdk != {str(x): file_hash(x) for x in sdk_files}:
            raise ReplayError('sdk_changed_during_build')
        executable = build / 'bin' / ('xparameter_replay_probe.exe' if os.name == 'nt'
                                      else 'xparameter_replay_probe')
        for name, path in source_files.items():
            if file_hash(path) != report['source_sha256'][name]:
                raise ReplayError('tool_source_changed_during_build')
        report['probe'] = {'path': str(executable), 'sha256': file_hash(executable)}
        errors = []
        for index, file_id in enumerate(FILE_IDS):
            record = {'file_id': file_id, 'status': 'failed'}
            report['files'].append(record)
            try:
                data, file_identity = read_once(installation / RELATIVE_DIRECTORY / file_id,
                                                installation)
                record['identity'] = file_identity
                record['bytes'] = len(data)
                record['sha256'] = sha256(data)
                raw = run_command([str(executable), '--json', '--stdin'], output,
                    'file-' + str(index + 1), report['commands'], stdin=data,
                    timeout=args.timeout, stdout_limit=MAX_SUMMARY_BYTES,
                    stderr_limit=64 * 1024, env=env)
                del data
                summary = strict_json(raw)
                if type(summary) is not dict or set(summary) != NATIVE_KEYS:
                    raise ReplayError('native_summary_schema')
                summary.update(file_id=file_id, bytes=record['bytes'], sha256=record['sha256'])
                encoded = (json.dumps(summary, ensure_ascii=True, allow_nan=False,
                    separators=(',', ':')) + '\n').encode('utf-8')
                if len(encoded) > MAX_SUMMARY_BYTES:
                    raise ReplayError('enriched_summary_byte_limit')
                record['summary'] = summary
                record['status'] = 'parsed_not_yet_scored'
                save(output / ('file-' + str(index + 1) + '.summary.json'), encoded)
            except (ReplayError, OSError, ValueError) as exc:
                record['error'] = str(exc) if isinstance(exc, ReplayError) else type(exc).__name__
                errors.append(file_id)
        if errors:
            raise ReplayError('one_or_more_selected_files_failed')
        totals = score_selected([x['summary'] for x in report['files']], expected)
        report.update(totals)
        for record in report['files']:
            record['status'] = 'exact_evidence_match'
        report['status'] = 'selected_installed_file_replay_pass'
    except (ReplayError, OSError, ValueError) as exc:
        report['status'] = 'blocked' if isinstance(exc, (Blocked, FileNotFoundError)) else 'failed'
        report['error'] = str(exc) if isinstance(exc, ReplayError) else type(exc).__name__
    finally:
        report['ended_utc'] = now()
        save_json(output / 'replay.json', report)
    return report

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installation-root', required=True)
    parser.add_argument('--sdk-prefix', required=True,
                        help='installed SDK with lib/cmake/RFModel; no source fallback')
    parser.add_argument('--output-dir', required=True, help='new exclusive directory; parent must exist')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--compiler', help='optional explicit installed C++ compiler')
    parser.add_argument('--generator', help='optional explicit CMake generator')
    parser.add_argument('--config', choices=('Debug', 'Release'), default='Release')
    parser.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args(argv)
    if not 0 < args.timeout <= 300:
        parser.error('--timeout must be in (0,300]')
    try:
        report = collect(args)
    except (OSError, ReplayError) as exc:
        print('Replay refused: ' + type(exc).__name__, file=sys.stderr)
        return 2
    print(json.dumps({'status': report['status'], 'independent_simulations': 0}))
    return 0 if report['status'] == 'selected_installed_file_replay_pass' else 1

if __name__ == '__main__':
    raise SystemExit(main())
