"""Freeze commands or archive the four-run OP1dB pilot; never execute SystemVue."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PureWindowsPath

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PLAN = ROOT / 'validation/rfamp-op1db-pilot-plan.json'
SOURCES = ('capture-cascade-reference.ps1', 'run-systemvue-reference.py',
           'compare-cascade-amplifier.py', 'archive-cascade-reference.py',
           'archive-cascade-input-reference.py', 'rfamp-parameter-pilot.py',
           'compare-amplifier-terms.py', 'compare-two-tone.py', 'compare-single-compression.py',
           'reference_records.py', 'retired_binding.py')
RUNS = (('A-baseline', 20, False, 0), ('B-op1db21', 21, False, 0),
        ('C-two-tone-holdout', 21, True, 90), ('D-restore-baseline', 20, False, 0))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def frozen_plan():
    data = PLAN.read_bytes()
    require(digest(data) == PLAN_SHA256, 'Frozen pilot plan changed')
    return json.loads(data)


def commands(windows_root, output_root):
    root, output = PureWindowsPath(windows_root), PureWindowsPath(output_root)
    require(root.is_absolute() and output.is_absolute(), 'Use absolute Windows paths')
    result = []
    for name, op1db, two, phase in RUNS:
        command = ['python', str(root / 'scripts/reference/run-systemvue-reference.py'),
                   'cascade', str(root / 'build-reference/RFModel_CascadeIntermods.wsv'),
                   str(output / name), '--source-power-dbm', '5', '--cascade-first-op1db-dbm', str(op1db),
                   '--cascade-phase-deg', str(phase), '--cascade-max-order', '11',
                   '--cascade-second-gain-db', '-10', '--cascade-riso-db', '140',
                   '--cascade-channel-bandwidth-hz', '1000000', '--cascade-disable-spectrum-reduction']
        if two:
            command += ['--cascade-two-tone', '--cascade-second-power-dbm', '5']
        result.append({'run_id': name, 'command': command})
    return result


def prepare(output, windows_root, windows_output):
    plan = frozen_plan()
    sources = {name: digest((HERE / name).read_bytes()) for name in SOURCES}
    manifest = {'schema_version': 1, 'classification': 'offline_preparation_only',
                'plan_sha256': PLAN_SHA256, 'plan': plan, 'source_sha256': sources,
                'windows_root': str(PureWindowsPath(windows_root)),
                'windows_output': str(PureWindowsPath(windows_output)),
                'runs': commands(windows_root, windows_output)}
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'plan.json').write_bytes(PLAN.read_bytes())
    def quote(text):
        return "'" + text.replace("'", "''") + "'"
    for run in manifest['runs']:
        # Separate scripts deliberately prevent blind four-run execution/retries.
        lines = ["$ErrorActionPreference = 'Stop'"]
        for name, expected in sources.items():
            path = str(PureWindowsPath(windows_root) / 'scripts/reference' / name)
            lines += [f"if ((Get-FileHash -LiteralPath {quote(path)} -Algorithm SHA256).Hash.ToLowerInvariant() -ne '{expected}') {{ throw 'Frozen source changed: {name}' }}"]
        lines += ['& ' + ' '.join(quote(x) for x in run['command']),
                  "if ($LASTEXITCODE -ne 0) { throw 'Capture failed or timeout unresolved; stop and inspect, never resubmit automatically.' }"]
        (output / (run['run_id'] + '.ps1')).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return manifest


def verify_collector_command(command, windows_root, run):
    """Bind actual collector path/options to the frozen wrapper request, including defaults."""
    _, op1db, two, phase = run
    root = PureWindowsPath(windows_root)
    prefix = ['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'RemoteSigned']
    require(command[:5] == prefix, 'Unexpected collector invocation')
    expected = {'-File': root / 'scripts/reference/capture-cascade-reference.ps1',
                '-WorkspacePath': root / 'build-reference/RFModel_CascadeIntermods.wsv',
                '-SourcePowerDbm': 5, '-SourcePhaseDeg': phase, '-MaximumOrder': 11,
                '-SecondGainDb': -10, '-ChannelBandwidthHz': 1e6, '-SecondaryRangeDb': -50,
                '-ReverseIsolationDb': 140, '-FirstOp1dbDbm': op1db}
    switches = {'-DisableSpectrumReduction'}
    if two:
        switches.add('-TwoTone')
        expected['-SecondSourcePowerDbm'] = 5
    seen, index = set(), 5
    while index < len(command):
        flag = command[index]
        require(flag not in seen, 'Duplicate collector option')
        seen.add(flag)
        if flag in switches:
            index += 1
            continue
        require(flag in expected and index + 1 < len(command), 'Unexpected collector option')
        actual, value = command[index + 1], expected[flag]
        if isinstance(value, PureWindowsPath):
            require(PureWindowsPath(actual) == value, 'Collector path mismatch')
        else:
            require(float(actual) == value, 'Collector value mismatch')
        index += 2
    require(seen == set(expected) | switches, 'Missing collector option')


def archive_packet(preparation, runs_root, output):
    manifest = json.loads((Path(preparation) / 'manifest.json').read_text(encoding='utf-8'))
    require(manifest['plan_sha256'] == PLAN_SHA256 and manifest['plan'] == frozen_plan(), 'Pilot plan mismatch')
    require(digest((Path(preparation) / 'plan.json').read_bytes()) == PLAN_SHA256, 'Prepared plan changed')
    require(manifest['source_sha256'] == {name: digest((HERE / name).read_bytes()) for name in SOURCES}, 'Pilot source mismatch')
    require(manifest['runs'] == commands(manifest['windows_root'], manifest['windows_output']), 'Pilot identity/order/command mismatch')
    archive, inputs = load('archive-cascade-reference'), load('archive-cascade-input-reference')
    comparison = archive.comparison
    records, files, workspace_hash, last_returned = [], {}, None, None
    for name, op1db, two, phase in RUNS:
        directory = Path(runs_root) / name
        required = ('capture.json', 'status.json', 'stderr.log', 'collector.ps1', 'runner.py')
        contents = {filename: (directory / filename).read_bytes() for filename in required}
        raw = json.loads(contents['capture.json'].decode('utf-8-sig'))
        status = json.loads(contents['status.json'].decode('utf-8-sig'))
        verify_collector_command(status['command'], manifest['windows_root'], (name, op1db, two, phase))
        raw_hash = digest(contents['capture.json'])
        reduced = archive.extract(raw, status, raw_hash)
        expected = dict(first_op1db_dbm=op1db, source_power_dbm=5, source_phase_deg=phase,
                        maximum_order=11, second_gain_db=-10, reverse_isolation_db=140,
                        channel_bandwidth_hz=1e6, two_tone=two, secondary_spectrum=False,
                        secondary_range_db=-50, spectrum_reduction=False, second_source_power_dbm=5)
        require(all(type(raw.get(k)) is type(v) if isinstance(v, bool) else True for k, v in expected.items()), 'Pilot boolean type mismatch')
        require(all(raw.get(k) == v for k, v in expected.items()), 'Pilot run parameters/identity mismatch: ' + name)
        require(workspace_hash is None or workspace_hash == raw['workspace_sha256'], 'Workspace bytes changed during pilot')
        workspace_hash = raw['workspace_sha256']
        # ISO UTC timestamps already checked by archive.extract; normalize precision for ordering.
        started = comparison.runner.datetime.fromisoformat(raw['run_started_utc'].replace('Z', '+00:00'))
        returned = comparison.runner.datetime.fromisoformat(raw['run_returned_utc'].replace('Z', '+00:00'))
        require(last_returned is None or started > last_returned, 'Pilot run order/overlap mismatch')
        last_returned = returned
        for saved, source in (('collector.ps1', SOURCES[0]), ('runner.py', SOURCES[1])):
            expected_hash = manifest['source_sha256'][source]
            require(digest(contents[saved]) == expected_hash and status['source_sha256'][saved] == expected_hash, 'Acquisition source mismatch')
        script_hash = digest(raw['submitted_script'].encode('utf-8'))
        require(status['submitted_script_sha256'] == script_hash, 'Submitted script hash mismatch')
        source_reference = inputs.extract(raw, status, raw_hash)
        # Retain complete port-1 arrays, dimensions, identities, and any complex impedances.
        # The existing input-reference format additionally provides measured source band edges.
        for key in ('F1', 'P1', 'V1', 'Z1', 'ID1'):
            reduced['nodes'].append(comparison.node(raw, comparison.SPECTRUM + key))
        comparison.spectrum(reduced, 1)
        records.append({'run_id': name, 'capture': reduced, 'input_reference': source_reference,
                        'submitted_script_sha256': script_hash,
                        'files': {k: digest(v) for k, v in contents.items()}})
        files[name] = contents
    require(len({r['capture']['raw_capture_sha256'] for r in records}) == 4, 'Repeated raw capture bytes')
    packet = {'schema_version': 1, 'classification': 'acquisition_only_not_compatibility',
              'plan_sha256': PLAN_SHA256, 'source_sha256': manifest['source_sha256'], 'runs': records,
              'input_wave_convention': 'V1_peak/sqrt(2*50 ohm); never nominal power or sqrt(P1)'}
    # Validate all inputs before creating a new destination. Never overwrite evidence.
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    for name, contents in files.items():
        (output / name).mkdir()
        for filename, data in contents.items():
            (output / name / filename).write_bytes(data)
    (output / 'manifest.json').write_text(json.dumps(packet, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    (output / 'preparation.json').write_bytes((Path(preparation) / 'manifest.json').read_bytes())
    (output / 'plan.json').write_bytes(PLAN.read_bytes())
    return packet


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    prepare_parser = sub.add_parser('prepare')
    prepare_parser.add_argument('--windows-root', required=True)
    prepare_parser.add_argument('--windows-output', required=True)
    prepare_parser.add_argument('--output', required=True, type=Path)
    archive_parser = sub.add_parser('archive')
    archive_parser.add_argument('--preparation', required=True, type=Path)
    archive_parser.add_argument('--runs', required=True, type=Path)
    archive_parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.mode == 'prepare':
        prepare(args.output, args.windows_root, args.windows_output)
    else:
        archive_packet(args.preparation, args.runs, args.output)


PLAN_SHA256 = '1436ad0e2139919882de558861f049d2c3e5ec67332b13a25f5bbd74158937c8'
if __name__ == '__main__':
    main()
