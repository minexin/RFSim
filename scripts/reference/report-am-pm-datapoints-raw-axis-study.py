"""Replay the frozen raw-axis scorer after verifying the acquisition archive."""

import argparse
import importlib.util
import json
from pathlib import Path


def module(filename):
    spec = importlib.util.spec_from_file_location(filename, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


archive = module('archive-am-pm-datapoints-raw-axis-acquisition.py')
scorer = module('diagnose-am-pm-datapoints-raw-axis-study.py')


def report(root, plan_path, observations_path, manifest_path, *, raw_root=None):
    verification = archive.verify_committed(root, plan_path, manifest_path, observations_path)
    if raw_root is not None:
        verification = archive.verify_archive(raw_root, plan_path, manifest_path, observations_path)
    plan, observations = scorer.load(plan_path), scorer.load(observations_path)
    result = scorer.score(plan, observations['samples'])
    reference_path = Path(root) / scorer.REFERENCE['path']
    scorer.require(scorer.digest(reference_path) == scorer.REFERENCE['sha256'], 'Historical baseline changed')
    result['baseline'] = scorer.compare_baseline(plan, observations['samples'][0], scorer.load(reference_path))
    inputs = []
    control_input = observations['samples'][0]['ordinary']['input_power_w']
    for profile, sample in zip(plan['profiles'], observations['samples']):
        actual = sample['ordinary']['input_power_w'] / control_input
        expected = 10 ** (profile['ordinary_output_target_dbm'] / 10)
        error = scorer.relative(actual, expected)
        inputs.append(dict(profile=profile['name'], measured_input_ratio=actual,
                           registered_input_ratio=expected, relative_error=error,
                           passed=error <= scorer.INPUT_TOLERANCE))
    result.update(plan_sha256=scorer.digest(plan_path), observations_sha256=scorer.digest(observations_path),
                  capture_manifest_sha256=scorer.digest(manifest_path), acquisition_archive_verified=True,
                  raw_sources_verified=verification['raw_source_bytes_verified'],
                  acquisition_verification=verification, input_identity_audit=inputs,
                  baseline_file_hashes_verified=True,
                  reporting_scope='All preregistered observations retained; hypothesis failures are descriptive, not archive failures.')
    rows = result['reproduction_controls']['rows'] + result['new_prediction_checks']['rows']
    result['observed_zero_increment_profiles'] = [row['profile'] for row in rows if row['zero_increment']]
    result['observed_exact_dcp_one_milliwatt_profiles'] = [
        row['profile'] for row in rows if row['dcp_output_power_w'] == .001]
    result['limitations'].extend([
        'A zero increment at a registered nonzero target contradicts the frozen nominal-zero-only rule; no replacement threshold is fitted here.',
        'Only the sampled powers establish zero/nonzero behavior; no exact boundary, symmetry, width or internal coordinate is inferred.',
        'Committed verification does not include full vendor workspace or raw-capture bytes; --raw-root is required to check those local files.',
    ])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('observations', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--capture-manifest', required=True, type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--raw-root', type=Path)
    args = parser.parse_args()
    scorer.require(not args.output.exists(), 'Output exists')
    result = report(args.root, args.plan, args.observations, args.capture_manifest, raw_root=args.raw_root)
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(dict(new_prediction_checks=result['new_prediction_checks']['summary'],
                          observed_zero_increment_profiles=result['observed_zero_increment_profiles'],
                          baseline_passed=result['baseline']['all_comparisons_matched'],
                          repeated_controls_passed=result['all_repeated_sweeps_matched'],
                          raw_sources_verified=result['raw_sources_verified'])))
    return 0 if result['baseline']['all_comparisons_matched'] and result['all_repeated_sweeps_matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
