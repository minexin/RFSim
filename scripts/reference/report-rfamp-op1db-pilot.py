"""Replay the completed registered OP1dB pilot using frozen B coefficients."""
import argparse
import importlib.util
from pathlib import Path


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


analysis = load('analyze-rfamp-op1db-pilot.py')
independent = load('audit-rfamp-op1db-holdout.py')
require = analysis.require
PREFIX = 'rfamp-op1db-pilot-'
FREEZE_SHA256 = '882094c660e6a9f60e68a59f928ffe67d30614b762d0edc786ac59a66bab0d6a'
REGISTRATION_SHA256 = '8cc5fbcbb7d389ddc9474cf27a029169154229b8c396482a553ff84bee2a8605'


def replay(native, evidence_directory, raw_runs=None):
    directory = Path(evidence_directory)
    paths = {key: directory / f'{PREFIX}{key}-20261015.json'
             for key in ('ab', 'freeze', 'registration', 'c', 'c-score', 'd', 'completion')}
    records = {key: analysis.read_json(path) for key, path in paths.items()}
    hashes = {key: analysis.digest(path.read_bytes()) for key, path in paths.items()}
    require(hashes['freeze'] == FREEZE_SHA256 and hashes['registration'] == REGISTRATION_SHA256,
            'Pre-C registration/freeze bytes changed')
    frozen, registration, completion = (records[k] for k in ('freeze', 'registration', 'completion'))
    require(hashes['ab'] == frozen['ab_archive_sha256'] == registration['ab_archive_sha256'],
            'A/B archive differs from registration')
    require(all(hashes[key] == value for key, value in completion['formal_files_sha256'].items()),
            'Completed evidence file changed')
    require(registration['analyses_submitted'] == 2 and not registration['C_acquired']
            and not registration['D_acquired'] and completion['analyses_submitted'] == 4,
            'Invalid acquisition accounting')
    a, b = records['ab']['runs']
    c, d = records['c']['run'], records['d']['run']
    require(analysis.validate_ab(a, b) == frozen['A_B_parameter_differences'], 'A/B contract changed')
    times = [a['capture']['run_started_utc'], a['capture']['run_returned_utc'],
             b['capture']['run_started_utc'], b['capture']['run_returned_utc'],
             frozen['created_utc'], registration['registered_utc'],
             completion['pre_C_guard']['utc'], c['capture']['run_started_utc'],
             c['capture']['run_returned_utc'], d['capture']['run_started_utc'],
             d['capture']['run_returned_utc'], completion['session_restored_utc']]
    require(all(analysis.utc(x) < analysis.utc(y) for x, y in zip(times, times[1:])),
            'Acquisition/freeze/control timeline changed')
    original_score = records['c-score']
    require(original_score['freeze_sha256'] == hashes['freeze']
            and original_score['c_archive_sha256'] == hashes['c']
            and original_score['native_replay']['native_replay_execution']
                == frozen['native_replay']['native_replay_execution'],
            'Original holdout score is not bound to the frozen runtime and observations')
    require(completion['pre_C_guard']['runtime'] == frozen['native_replay']['native_replay_execution'],
            'Pre-C guard runtime differs from freeze')
    for packet in (a, b, c, d):
        require(packet['files_sha256'] == completion['raw_runs_sha256'][packet['run_id']],
                'Completed raw run identity changed')
        analysis.units_index(packet['parameter_units'], packet['capture'])

    # The only primary prediction call uses the unchanged preregistered scorer.
    # Do not call identify() or freeze() after C acquisition.
    c_score = analysis.score_c(native, frozen, c)
    d_score = analysis.score_d(a, c, d)
    primary = c_score['measured_input_prediction']
    historical = original_score['measured_input_prediction']
    for key in ('expected_origin_count', 'recorded_origin_count', 'passed_origin_count',
                'failed_origin_count', 'missing_origins', 'unexpected_origins', 'complete_and_passed'):
        require(primary[key] == historical[key], 'Native replay changed the recorded C outcome')
    require([t['label'] for t in primary['checks']] == [t['label'] for t in historical['checks']],
            'Native replay changed C term identities')
    require(d_score == records['d']['score'], 'End-control replay changed')
    audit = independent.audit(frozen, (a, b, c, d), raw_runs)
    files = [Path(__file__), Path(independent.__file__)] + [analysis.HERE / n for n in analysis.LOGIC_SOURCES]
    return {'schema_version': 1, 'classification': 'completed_RFAMP_OP1dB_pilot_not_full_compatibility',
            'evidence_sha256': hashes, 'pre_C_commit': completion['pre_C_guard']['pre_C_commit'],
            'relative_complex_wave_tolerance': 1e-7, 'analyses_submitted': 4,
            'coefficient_refit_on_C': False, 'independent_prediction_origins': 380,
            'C': c_score, 'D': d_score, 'independent_audit': audit,
            'passed': primary['complete_and_passed'] and d_score['all_controls_passed'] and audit['passed'],
            'full_RFAMP_compatibility': False, 'proprietary_coefficient_law_identified': False,
            'native_replay': native.manifest(list(paths.values()), files),
            'limits': frozen['limits']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-probe', required=True, type=Path)
    parser.add_argument('--evidence-directory', default=analysis.ROOT / 'validation', type=Path)
    parser.add_argument('--raw-runs', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Report output already exists')
    result = replay(analysis.NativeReplay(args.native_probe), args.evidence_directory, args.raw_runs)
    analysis.write_new(args.output, result)
    import json
    print(json.dumps({'passed': result['passed'], 'independent_prediction_origins': 380,
                      'C_max_relative_wave_error': result['C']['measured_input_prediction']['max_relative_wave_error'],
                      'D_all_controls_passed': result['D']['all_controls_passed'],
                      'independent_audit': result['independent_audit']}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
