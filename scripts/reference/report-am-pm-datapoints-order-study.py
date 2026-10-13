"""Verify the completed order-study archive and replay its frozen scientific scorer."""

import argparse
import importlib.util
import json
from pathlib import Path


def module(filename):
    spec = importlib.util.spec_from_file_location('order_report_' + filename.replace('-', '_'),
                                                 Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


archive = module('archive-am-pm-datapoints-order-acquisition.py')
scorer = module('diagnose-am-pm-datapoints-order-study.py')


def report(root, plan_path, observations_path, manifest_path, *, raw_root=None):
    verification = archive.verify_committed(root, plan_path, manifest_path, observations_path)
    if raw_root is not None:
        verification = archive.verify_archive(raw_root, plan_path, manifest_path, observations_path)
    plan, observations = scorer.load(plan_path), scorer.load(observations_path)
    reference_path = Path(root) / scorer.REFERENCE['path']
    scorer.require(scorer.digest(reference_path) == scorer.REFERENCE['sha256'], 'Historical reference changed')
    result = scorer.score(plan, observations['samples'], historical_observations=scorer.load(reference_path))
    scorer.require(result['all_planned_profiles_present'] is True and result['observed_profile_count'] == 10,
                   'Completed report requires all ten registered observations')
    scorer.require(result['eligible_for_vendor_compatibility'] is False
                   and result['eligible_for_full_model_compatibility'] is False
                   and result['product_formula_changed'] is False, 'Frozen scientific scope changed')
    rows = {}
    for group in ('historical_reproductions', 'new_prediction_checks', 'order_reproductions'):
        for row in result[group]['rows']:
            scorer.require(row['profile'] not in rows, 'Duplicate ordinary report row')
            rows[row['profile']] = row
    scorer.require(set(rows) == {profile['name'] for profile in plan['profiles']},
                   'Incomplete ordinary report rows')
    # The role groups interleave in the acquisition sequence; retain that order.
    ordered_rows = [rows[profile['name']] for profile in plan['profiles']]
    result.update(
        plan_sha256=scorer.digest(plan_path), observations_sha256=scorer.digest(observations_path),
        capture_manifest_sha256=scorer.digest(manifest_path), historical_reference_sha256=scorer.digest(reference_path),
        acquisition_archive_verified=True, acquisition_verification=verification,
        raw_sources_verified=verification['raw_source_bytes_verified'], baseline_file_hashes_verified=True,
        observed_zero_increment_profiles=[row['profile'] for row in ordered_rows if row['zero_increment']],
        observed_exact_dcp_one_milliwatt_profiles=[row['profile'] for row in ordered_rows
                                                 if row['dcp_output_power_w'] == .001],
        reporting_scope='All ten registered observations are retained. Hypothesis failures and historical or paired-order differences are descriptive; acquisition validity, both zero baselines and repeated sweeps are gates.',
    )
    result['limitations'].append(
        'Committed verification does not reopen full vendor workspaces or raw captures; --raw-root is required to verify those retained local files.')
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
    text = json.dumps(result, indent=2, allow_nan=False) + '\n'
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    print(json.dumps(dict(
        new_prediction_checks=result['new_prediction_checks']['summary'],
        completed_order_pairs=result['order_comparisons']['completed_pair_count'],
        complete_sequence_replication_matched=result['complete_sequence_replication_matched'],
        observed_zero_increment_profiles=result['observed_zero_increment_profiles'],
        baseline_gates_passed=result['all_observed_zero_baselines_matched'],
        repeated_sweep_gates_passed=result['all_repeated_sweeps_matched'],
        raw_sources_verified=result['raw_sources_verified'],
    )))
    return 0 if (result['all_planned_profiles_present'] is True
                 and len(result['zero_baseline_controls']) == 2
                 and result['all_observed_zero_baselines_matched'] is True
                 and result['all_repeated_sweeps_matched'] is True) else 1


if __name__ == '__main__':
    raise SystemExit(main())
