"""Score the registered two-order replication without running SystemVue or fitting data."""

import argparse
import importlib.util
import json
import math
from pathlib import Path


def _module(filename):
    spec = importlib.util.spec_from_file_location('order_' + filename.replace('-', '_'),
                                                 Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


previous = _module('diagnose-am-pm-datapoints-raw-axis-study.py')
require, number, load, digest = previous.require, previous.number, previous.load, previous.digest
relative, identical = previous.relative, previous.identical
PHASE_TOLERANCE = 1e-5
POWER_TOLERANCE = 1e-7
INPUT_TOLERANCE = 1e-10
REPLICATION_POWER_TOLERANCE = 1e-10
PLAN_SHA256 = '3fd61758408c87d1fb3e163ceee04a8ceb9b75f77a880ae4ea3a8c0ab2e58de6'
REFERENCE = dict(path='validation/am-pm-datapoints-raw-axis-observations-20261013.json',
                 sha256='7fed2f4f3ce455c0a1491d76e1d7c9a864dfb1ecc264acca29369557272da512')
HYPOTHESES = previous.HYPOTHESES
PHYSICS = previous.PHYSICS
SPECIFICATIONS = (
    ('a-zero', 0, 'historical_reproduction', 'a', 0),
    ('a-positive-epsilon', 1e-6, 'historical_reproduction', 'a', 1),
    ('a-positive-delta', 1e-5, 'new_prediction_check', 'a', 2),
    ('a-negative-delta', -1e-5, 'new_prediction_check', 'a', 3),
    ('a-negative-epsilon', -1e-6, 'historical_reproduction', 'a', 4),
    ('b-zero', 0, 'historical_reproduction', 'b', 0),
    ('b-negative-epsilon', -1e-6, 'historical_reproduction', 'b', 1),
    ('b-negative-delta', -1e-5, 'order_reproduction', 'b', 2),
    ('b-positive-delta', 1e-5, 'order_reproduction', 'b', 3),
    ('b-positive-epsilon', 1e-6, 'historical_reproduction', 'b', 4),
)
ORDER_PAIRS = [[name, 'b-' + name[2:]] for name, *_ in SPECIFICATIONS[:5]]
HISTORICAL_PROFILES = {0: 'control-zero', -1e-6: 'negative-0p000001', 1e-6: 'positive-0p000001'}
HYPOTHESIS_DEFINITIONS = {
    'dcp_continuous': 'Natural cubic spline evaluated on measured ordinary DCP output dBm, added to measured ordinary source phase.',
    'raw_continuous': 'Same spline evaluated on ordinary raw spectral output power converted to dBm; measured ordinary source phase added.',
    'raw_nominal_zero_bypass': 'Same raw-power spline; set only its increment to zero at the registered nominal zero-dBm control, without assuming raw power is exactly 0.001 W.',
    'raw_report_zero_bypass': 'Same raw-power spline; set its increment to zero wherever ordinary DCP output power equals exactly 0.001 W.',
}


def validate_plan(plan):
    require(isinstance(plan, dict), 'Plan must be an object')
    require(type(plan['schema_version']) is int and plan['schema_version'] == 1, 'Wrong plan schema')
    for key, expected in (
        ('phase_absolute_tolerance_degrees', PHASE_TOLERANCE),
        ('baseline_power_relative_tolerance', POWER_TOLERANCE),
        ('input_power_relative_tolerance', INPUT_TOLERANCE),
        ('ordinary_replication_power_relative_tolerance', REPLICATION_POWER_TOLERANCE),
    ):
        require(number(plan[key]) == expected, 'Registered tolerance changed: ' + key)
    require(plan['classification'] == 'preregistered_near_zero_order_and_historical_replication',
            'Study classification changed')
    require(plan['analysis_target'] == previous.normalizer.ANALYSIS, 'Wrong analysis target')
    require(plan['source_workspace_sha256'] ==
            'ead90a212c786eff0cfa782dab47dbfe14c421b3f43b32c573bb990cb8348877', 'Wrong source workspace')
    require(plan['baseline_reference'] == REFERENCE and plan['baseline_profile'] == 'control-zero',
            'Historical source identity changed')
    require(plan['historical_target_profiles'] == {
        '0': 'control-zero', '-0.000001': 'negative-0p000001', '0.000001': 'positive-0p000001'},
        'Historical target mapping changed')
    require(identical(plan['analysis_budget'], dict(profiles=10, reserved_setup_recovery=0, total_maximum=10)),
            'Analysis budget changed')
    require(identical(plan['source_phase_conventions'], dict(ordinary='measured_input_node_phase',
            sweep='measured_input_node_phase', nominal_sweep_source_phase_degrees=0)),
            'Source phase ownership changed')
    require(plan['hypotheses'] == list(HYPOTHESES)
            and plan['hypothesis_definitions'] == HYPOTHESIS_DEFINITIONS, 'Registered hypotheses changed')
    polynomial = plan['local_spline_polynomial']
    for key, expected in (('domain_dbm', [-5, 5]),
                          ('ascending_coefficients', [33.4375, 3.5625, -.3375, -.0225])):
        require(isinstance(polynomial[key], list) and [number(v) for v in polynomial[key]] == expected,
                'Analytic spline changed')
    primary = plan['primary_evaluation']
    require(identical(primary, dict(historical_reproduction_count=6, new_prediction_check_count=2,
        order_reproduction_count=2, paired_target_count=5,
        new_prediction_profile_names=['a-positive-delta', 'a-negative-delta'],
        zero_increment_test='Circular difference between measured ordinary output and input phase no greater than 1e-5 degrees.',
        source_and_report_axes_separate=True, controls_excluded_from_new_prediction_pass_rates=True,
        independent_new_sweep_prediction_points=0)), 'Primary/control scope changed')
    order = plan['order_comparisons']
    require(order['pairs'] == ORDER_PAIRS and order['fields'] == [
        'input_power_w', 'output_power_w', 'raw_output_w', 'input_phase_rad', 'output_phase_rad', 'phase_increment'],
        'Registered order comparisons changed')
    require(number(order['ordinary_power_relative_tolerance']) == REPLICATION_POWER_TOLERANCE
            and number(order['ordinary_phase_absolute_tolerance_degrees']) == PHASE_TOLERANCE
            and order['mismatches_are_results_not_acquisition_failures'] is True,
            'Order comparison tolerance or role changed')
    quick = plan['quick_sweeps']
    for key, expected in (('planned_points_per_call', 61), ('maximum_extra_endpoints_per_call', 1),
                          ('total_planned_observations', 610)):
        require(type(quick[key]) is int and quick[key] == expected, 'Repeated-sweep budget changed')
    require(quick['required_parameter_readback'] == previous.normalizer.SWEEP + 'Enable == 1',
            'Sweep enable guard changed')
    require(set(plan['input_power_identity']) == {'ordinary', 'repeated_sweep', 'source_parameter', 'baseline_input'}
            and all(isinstance(v, str) and v for v in plan['input_power_identity'].values()),
            'Missing input identity rules')
    for text in (plan['purpose'], plan['historical_hypothesis_status'], polynomial['source'],
                 order['interpretation'], quick['interpretation'], quick['repeated_sweep_gate']):
        require(isinstance(text, str) and bool(text), 'Missing registered interpretation')
    for key in ('acquisition_rules', 'exclusions'):
        require(isinstance(plan[key], list) and plan[key]
                and all(isinstance(v, str) and v for v in plan[key]), 'Missing registered scope')
    profiles = plan['profiles']
    require(isinstance(profiles, list) and len(profiles) == 10, 'Exactly ten ordered profiles required')
    for profile, (name, target, role, block, position) in zip(profiles, SPECIFICATIONS):
        expected = dict(PHYSICS, name=name, ordinary_output_target_dbm=target,
                        ordinary_role=role, block=block, block_position=position)
        require(isinstance(profile, dict) and set(profile) == set(expected), 'Registered profile fields changed')
        for key, wanted in expected.items():
            actual = profile[key]
            if isinstance(wanted, list):
                require(isinstance(actual, list) and [number(v) for v in actual] == wanted,
                        'Registered table changed')
            elif isinstance(wanted, str):
                require(isinstance(actual, str) and actual == wanted, 'Profile order/block/role changed')
            else:
                require(number(actual) == wanted, 'Registered parameter changed: ' + key)
        require(type(profile['expected_points']) is int and type(profile['block_position']) is int,
                'Profile count/position must be integer')
    return profiles


def validate_samples(plan, samples, *, allow_partial=False):
    profiles = validate_plan(plan)
    require(type(allow_partial) is bool, 'Partial mode must be boolean')
    require(isinstance(samples, list) and 0 < len(samples) <= len(profiles), 'Empty or excessive observations')
    require(allow_partial or len(samples) == len(profiles), 'Incomplete profile collection')
    for index, (sample, profile) in enumerate(zip(samples, profiles)):
        previous._validate_sample(sample, profile)
        control_index = 0 if profile['block'] == 'a' else 5
        require(control_index <= index, 'Block zero control must precede its observations')
        control = samples[control_index]['ordinary']['input_power_w']
        ratio = sample['ordinary']['input_power_w'] / control
        expected_ratio = 10 ** (profile['ordinary_output_target_dbm'] / 10)
        require(relative(ratio, expected_ratio) <= INPUT_TOLERANCE,
                'Ordinary registered input-power ratio mismatch within block ' + profile['block'])
    return profiles


def _registered_sample(plan, sample):
    profiles = validate_plan(plan)
    matches = [profile for profile in profiles if profile['name'] == sample['profile']['name']]
    require(len(matches) == 1, 'Unregistered observation profile')
    previous._validate_sample(sample, matches[0])
    return matches[0]


def _historical_samples(observations):
    require(isinstance(observations, dict) and observations['plan_sha256'] == previous.PLAN_SHA256,
            'Historical observation plan identity changed')
    samples = observations['samples']
    require(isinstance(samples, list) and len(samples) == len(previous.NAMES), 'Incomplete historical observations')
    result = {}
    for index, (sample, name, target) in enumerate(zip(samples, previous.NAMES, previous.TARGETS)):
        profile = dict(PHYSICS, name=name, ordinary_output_target_dbm=target,
                       ordinary_role='reproduction_control' if index == 0 else 'new_prediction_check')
        previous._validate_sample(sample, profile)
        if target in HISTORICAL_PROFILES:
            require(name == HISTORICAL_PROFILES[target], 'Historical target identity changed')
            result[target] = sample
    require(set(result) == set(HISTORICAL_PROFILES), 'Incomplete historical target coverage')
    return result


def compare_baseline(plan, new_sample, prior_observations):
    """Both block-zero profiles use the same complete historical baseline gate."""
    profile = _registered_sample(plan, new_sample)
    require(profile['name'] in ('a-zero', 'b-zero') and profile['block_position'] == 0,
            'Historical baseline gate requires a registered block zero')
    old = _historical_samples(prior_observations)[0]
    rows = [previous._comparison(new_sample['ordinary'], old['ordinary'], 'ordinary', 0,
                                 input_tolerance=POWER_TOLERANCE)]
    rows += previous._sweep_comparisons(new_sample, old, input_tolerance=POWER_TOLERANCE)
    return dict(classification='same_configuration_same_mode_complete_baseline',
                profile=profile['name'], block=profile['block'], reference_profile=old['profile']['name'],
                comparison_count=len(rows), all_comparisons_matched=all(row['matched'] for row in rows), rows=rows,
                full_historical_sweep_reproduced=all(row['matched'] for row in rows if row['category'] != 'ordinary'),
                vendor_compatibility_proven=False)


def compare_repeated_sweep(plan, new_sample, control_sample):
    """Every later sweep, including b-zero, is checked against a-zero."""
    profiles = validate_plan(plan)
    previous._validate_sample(control_sample, profiles[0])
    profile = _registered_sample(plan, new_sample)
    rows = previous._sweep_comparisons(new_sample, control_sample, input_tolerance=INPUT_TOLERANCE)
    return dict(classification='repeated_control_sweep_not_independent_prediction_checks',
                profile=profile['name'], reference_profile=control_sample['profile']['name'],
                comparison_count=len(rows), all_comparisons_matched=all(row['matched'] for row in rows), rows=rows)


def _ordinary_comparison(new, old):
    result = previous._comparison(new, old, 'ordinary', 0, input_tolerance=REPLICATION_POWER_TOLERANCE)
    # The registered replication tolerance applies to all three ordinary powers.
    # Baseline and sweep gates retain the previous module's separate 1e-7 limit.
    for power in result['powers'].values():
        power['relative_tolerance'] = REPLICATION_POWER_TOLERANCE
        power['matched'] = power['relative_error'] <= REPLICATION_POWER_TOLERANCE
    result['matched'] = all(item['matched'] for item in list(result['powers'].values()) + list(result['phases'].values()))
    return result


def score(plan, samples, *, allow_partial=False, historical_observations=None):
    profiles = validate_samples(plan, samples, allow_partial=allow_partial)
    rows = [previous.score_point(profile, sample['ordinary']) for profile, sample in zip(profiles, samples)]
    groups = {}
    for key, role, count in (('historical_reproductions', 'historical_reproduction', 6),
                             ('new_prediction_checks', 'new_prediction_check', 2),
                             ('order_reproductions', 'order_reproduction', 2)):
        selected = [row for row in rows if row['role'] == role]
        groups[key] = dict(planned_count=count, observed_count=len(selected), missing_count=count - len(selected),
                           summary=previous.summarize(selected), rows=selected)
    input_audit = []
    for sample, profile in zip(samples, profiles):
        control = samples[0 if profile['block'] == 'a' else 5]
        actual = sample['ordinary']['input_power_w'] / control['ordinary']['input_power_w']
        expected = 10 ** (profile['ordinary_output_target_dbm'] / 10)
        error = relative(actual, expected)
        input_audit.append(dict(profile=profile['name'], block=profile['block'],
            control_profile=control['profile']['name'], measured_input_ratio=actual,
            registered_input_ratio=expected, relative_error=error, relative_tolerance=INPUT_TOLERANCE,
            passed=error <= INPUT_TOLERANCE))
    by_name = {sample['profile']['name']: sample for sample in samples}
    pairs, missing_pairs = [], []
    for left, right in ORDER_PAIRS:
        if left not in by_name or right not in by_name:
            missing_pairs.append([left, right])
            continue
        comparison = _ordinary_comparison(by_name[right]['ordinary'], by_name[left]['ordinary'])
        comparison.update(left_profile=left, right_profile=right,
                          nominal_output_dbm=by_name[left]['profile']['ordinary_output_target_dbm'])
        pairs.append(comparison)
    paired_match = bool(pairs) and all(row['matched'] for row in pairs)
    order = dict(planned_pair_count=5, completed_pair_count=len(pairs), missing_pair_count=len(missing_pairs),
                 missing_pairs=missing_pairs, all_completed_pairs_matched=paired_match,
                 complete_order_reproduction_matched=len(pairs) == 5 and paired_match,
                 mismatches_are_results_not_acquisition_failures=True, rows=pairs)
    history_rows, baselines = [], []
    if historical_observations is not None:
        historical = _historical_samples(historical_observations)
        for sample, profile in zip(samples, profiles):
            if profile['ordinary_role'] == 'historical_reproduction':
                old = historical[profile['ordinary_output_target_dbm']]
                comparison = _ordinary_comparison(sample['ordinary'], old['ordinary'])
                comparison.update(profile=profile['name'], reference_profile=old['profile']['name'],
                                  nominal_output_dbm=profile['ordinary_output_target_dbm'])
                history_rows.append(comparison)
            if profile['block_position'] == 0:
                baselines.append(compare_baseline(plan, sample, historical_observations))
    historical_match = bool(history_rows) and all(row['matched'] for row in history_rows)
    historical = dict(reference_provided=historical_observations is not None, planned_count=6,
                      completed_count=len(history_rows), missing_count=6 - len(history_rows),
                      all_completed_comparisons_matched=historical_match,
                      complete_historical_reproduction_matched=len(history_rows) == 6 and historical_match,
                      mismatches_are_results_not_acquisition_failures=True, rows=history_rows)
    repeats = [dict(profile=sample['profile']['name'], sweep=sample['sweep'],
                    additional_sweep_points=sample['additional_sweep_points'],
                    comparison=compare_repeated_sweep(plan, sample, samples[0]) if index else None)
               for index, sample in enumerate(samples)]
    sweep_match = all(item['comparison']['all_comparisons_matched'] for item in repeats[1:])
    baseline_match = bool(baselines) and all(item['all_comparisons_matched'] for item in baselines)
    complete = len(samples) == len(profiles)
    return dict(schema_version=1, classification='finite_near_zero_order_and_historical_replication',
        all_planned_profiles_present=complete, planned_profile_count=10, observed_profile_count=len(samples),
        missing_profile_count=10 - len(samples), missing_profile_names=[p['name'] for p in profiles[len(samples):]],
        sequence_status='complete' if complete else 'incomplete',
        complete_sequence_replication_matched=complete and len(baselines) == 2 and baseline_match
            and sweep_match and order['complete_order_reproduction_matched']
            and historical['complete_historical_reproduction_matched'],
        planned_sweep_point_count=610, observed_sweep_point_count=61 * len(samples),
        phase_absolute_tolerance_degrees=PHASE_TOLERANCE, hypotheses=list(HYPOTHESES),
        historical_hypothesis_status=plan['historical_hypothesis_status'], **groups,
        input_identity_audit=input_audit, order_comparisons=order, historical_comparisons=historical,
        zero_baseline_controls=baselines, all_observed_zero_baselines_matched=baseline_match,
        repeated_quick_sweeps=repeats, all_repeated_sweeps_matched=sweep_match,
        complete_repeated_sweep_collection=complete,
        additional_points_in_planned_scores=False, quick_sweeps_are_independent_prediction_checks=False,
        parameters_fitted_this_study=0, eligible_for_vendor_compatibility=False,
        eligible_for_full_model_compatibility=False, raw_sources_verified=False, product_formula_changed=False,
        limitations=list(plan['exclusions']) + [
            'Historical ordinary and paired-order differences are results; only invalid acquisitions, zero baselines and repeated sweeps gate acquisition.',
            'A partial prefix may pass every observed control while complete sequence replication remains unestablished.',
            'All four hypotheses already failed the prior complete experiment; these local results do not restore their general validity.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--allow-partial', action='store_true')
    args = parser.parse_args()
    require(not args.output.exists(), 'Output exists')
    require(digest(args.plan) == PLAN_SHA256, 'Frozen plan bytes changed')
    plan, evidence = load(args.plan), load(args.evidence)
    require(evidence['plan_sha256'] == PLAN_SHA256, 'Evidence references different plan bytes')
    reference = args.reference if args.reference is not None else args.root / REFERENCE['path']
    require(digest(reference) == REFERENCE['sha256'], 'Historical reference bytes changed')
    result = score(plan, evidence['samples'], allow_partial=args.allow_partial, historical_observations=load(reference))
    result.update(plan_sha256=PLAN_SHA256, evidence_sha256=digest(args.evidence),
                  historical_reference_sha256=digest(reference), baseline_file_hashes_verified=True)
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(dict(sequence_status=result['sequence_status'],
        observed_new_prediction_checks=result['new_prediction_checks']['observed_count'],
        completed_order_pairs=result['order_comparisons']['completed_pair_count'],
        baseline_passed=result['all_observed_zero_baselines_matched'],
        repeated_sweeps_passed=result['all_repeated_sweeps_matched'])))
    return 0 if result['all_observed_zero_baselines_matched'] and result['all_repeated_sweeps_matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
