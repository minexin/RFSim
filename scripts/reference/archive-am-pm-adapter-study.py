"""Archive the independent compressed/frequency holdouts, retaining warning labels."""
import argparse
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('study', Path(__file__).with_name('archive-am-pm-parameter-study.py'))
study = importlib.util.module_from_spec(spec)
spec.loader.exec_module(study)
archive, require = study.archive, study.require
PLAN_SHA = '3ed1d60cd5b6279c7c7da1f2e0c3324f8a1535085f4c6382a0d8d999745ca40c'


def normalized_source(root, source, profile):
    path = root/source['path']
    require(archive.digest(path) == source['sha256'], 'Capture changed')
    directory = root/source['call_directory']
    for filename, field in [('status.json', 'status_sha256'), ('run-intent.json', 'run_intent_sha256'),
                            ('before.json', 'before_sha256'), ('stderr.log', 'stderr_sha256')]:
        require(archive.digest(directory/filename) == source[field], 'Call evidence changed')
    capture = archive.load(path)
    status = archive.load(directory/'status.json')
    intent = archive.load(directory/'run-intent.json')
    require(status['profile'] == intent['profile'] == profile['name'] == source['profile'], 'Call identity changed')
    require(capture['manager_errors'] == source['diagnostics'], 'Diagnostics changed')
    if source['recovered_diagnostic']:
        require(status['exit_code'] == 1 and profile['name'] == '10GHz-saturation', 'Unexpected recovered run')
        require('driven past the input 1 dB compression point' in source['diagnostics'], 'Wrong warning')
        # The frozen helper checks IsAnalysisDone before throwing this exact
        # Manager.GetErrors message. Retain that inference and the warning.
        require(source['diagnostics'].strip() in (directory/'stderr.log').read_text(encoding='utf-8-sig'),
                'Warning is not the recorded failure')
        capture.update(profile=profile['name'], analysis_done=1,
                       run_started_utc=intent['run_started_utc'], run_returned_utc=status['finished_utc'])
        completion = 'Inferred from frozen helper control flow: IsAnalysisDone passed before Manager.GetErrors threw; return bound is supervisor completion.'
    else:
        require(status['exit_code'] == 0 and not source['diagnostics'], 'Capture failed')
        require(capture['run_started_utc'] == intent['run_started_utc'], 'Run start changed')
        completion = 'Direct completed-analysis capture with empty Manager.GetErrors.'
    result = study.normalize(capture, profile, allow_diagnostic=True, retain_extra_endpoint=True)
    result['completion_basis'] = completion
    result['capture_source'] = source
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('plan', type=Path)
    p.add_argument('amendment', type=Path)
    p.add_argument('manifest', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    require(not args.output.exists(), 'Output exists')
    require(archive.digest(args.plan) == PLAN_SHA, 'Preregistered plan changed')
    plan, amendment, manifest = map(archive.load, [args.plan, args.amendment, args.manifest])
    require(manifest['plan_sha256'] == amendment['plan_sha256'] == PLAN_SHA, 'Plan identity mismatch')
    require(manifest['amendment_sha256'] == archive.digest(args.amendment), 'Amendment changed')
    sources, profiles = manifest['captures'], amendment['effective_profiles']
    require(len(sources) == len(profiles) == 4, 'Four complete profiles required')
    rows = [normalized_source(args.root, source, profile) for source, profile in zip(sources, profiles)]
    result = dict(schema_version=1, plan_sha256=PLAN_SHA, amendment_sha256=archive.digest(args.amendment),
                  manifest_sha256=archive.digest(args.manifest), hypotheses=plan['hypotheses'],
                  phase_absolute_tolerance_degrees=1e-5, samples=rows,
                  classification='independent_compressed_frequency_holdouts_with_retained_deviations',
                  raw_source_bytes_verified=True, eligible_for_full_model_compatibility=False)
    with args.output.open('x', encoding='utf-8', newline='\n') as f:
        json.dump(result, f, indent=2, allow_nan=False)
        f.write('\n')
    print('Archived 148 registered points and', sum(len(r['additional_sweep_points']) for r in rows),
          'extra endpoints; warning-bearing profiles:', sum(bool(r['diagnostics']) for r in rows))


if __name__ == '__main__':
    main()
