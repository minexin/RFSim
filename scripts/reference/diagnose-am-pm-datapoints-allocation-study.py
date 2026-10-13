"""Score the frozen finite gain/source allocation study; no vendor calls or fitting."""

import argparse
from decimal import Decimal
import hashlib
import importlib.util
import json
import math
from pathlib import Path


def _module(filename):
    spec = importlib.util.spec_from_file_location('allocation_' + filename.replace('-', '_'),
                                                 Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


previous = _module('diagnose-am-pm-datapoints-raw-axis-study.py')
normalizer = _module('normalize-am-pm-datapoints-allocation.py')
require, number, load, digest = previous.require, previous.number, previous.load, previous.digest
relative, identical = previous.relative, previous.identical
PLAN_SHA256 = 'df5cf362e9fc2fb852965c5a18bcf5c9bb556b3387f7b40246eb0bb04cf6a91c'
# The API receives objects, so bind the entire typed registration independently
# of disk paths and whitespace. The CLI separately binds the original bytes.
PLAN_CANONICAL_SHA256 = '28b129b7242a83ec28d42e59a6d2f973c6c664a68535ed2af459c1c3220d014d'
REFERENCE = dict(path='validation/am-pm-datapoints-raw-axis-observations-20261013.json',
                 sha256='7fed2f4f3ce455c0a1491d76e1d7c9a864dfb1ecc264acca29369557272da512')
HYPOTHESES = ('output_discrete_pattern', 'input_discrete_pattern')
LEGACY_HYPOTHESES = previous.HYPOTHESES
PHASE_TOLERANCE = 1e-5
POWER_TOLERANCE = 1e-7
INPUT_TOLERANCE = 1e-10
CROSS_GAIN_INPUT_TOLERANCE = 1e-7
REPLICATION_POWER_TOLERANCE = 1e-10
HISTORICAL_PROFILES = {0: 'control-zero', -1e-6: 'negative-0p000001', 1e-6: 'positive-0p000001'}
OUTPUT_ZERO_SET = (Decimal('0'), Decimal('-0.000001'))
INPUT_ZERO_SET = (Decimal('-20'), Decimal('-20.000001'))


def validate_plan(plan):
    require(isinstance(plan, dict), 'Plan must be an object')
    try:
        encoded = json.dumps(plan, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')
    except (TypeError, ValueError) as error:
        raise ValueError('Invalid registered plan values') from error
    require(hashlib.sha256(encoded).hexdigest() == PLAN_CANONICAL_SHA256,
            'Registered allocation plan content changed')
    return plan['profiles']


def _validate_sample(sample, profile):
    previous._validate_sample(sample, profile)
    normalizer.validate_raw_input(sample)


def _registered_sample(plan, sample):
    profiles = validate_plan(plan)
    matches = [profile for profile in profiles if profile['name'] == sample['profile']['name']]
    require(len(matches) == 1, 'Unregistered allocation observation')
    _validate_sample(sample, matches[0])
    return matches[0]


def _input_audit(profile, sample, control, global_control):
    target = number(profile['ordinary_output_target_dbm'])
    source_dbm = Decimal(str(profile['ordinary_output_target_dbm'])) - Decimal(str(profile['gain_db']))
    result = dict(profile=profile['name'], gain_group=profile['gain_group'])
    for key, reference, exponent, tolerance in (
            ('within_gain', control, target / 10, INPUT_TOLERANCE),
            ('cross_gain', global_control, float((source_dbm + Decimal(20)) / Decimal(10)),
             CROSS_GAIN_INPUT_TOLERANCE)):
        actual = sample['ordinary']['input_power_w'] / reference['ordinary']['input_power_w']
        expected = 10 ** exponent
        error = relative(actual, expected)
        result[key] = dict(control_profile=reference['profile']['name'], measured_input_ratio=actual,
                           registered_input_ratio=expected, relative_error=error,
                           relative_tolerance=tolerance, passed=error <= tolerance)
    return result


def validate_samples(plan, samples, *, allow_partial=False):
    profiles = validate_plan(plan)
    require(type(allow_partial) is bool, 'Partial mode must be boolean')
    require(isinstance(samples, list) and 0 < len(samples) <= len(profiles), 'Empty or excessive observations')
    require(allow_partial or len(samples) == len(profiles), 'Incomplete profile collection')
    observed = {}
    for sample, profile in zip(samples, profiles):
        _validate_sample(sample, profile)
        observed[profile['name']] = sample
        require(profile['control_profile'] in observed, 'Same-gain control must precede its observations')
        audit = _input_audit(profile, sample, observed[profile['control_profile']], samples[0])
        require(audit['within_gain']['passed'], 'Ordinary input ratio mismatch within gain ' + profile['gain_group'])
        require(audit['cross_gain']['passed'], 'Ordinary input ratio mismatch across gains')
    return profiles


def _historical_samples(observations):
    require(isinstance(observations, dict) and observations['plan_sha256'] == previous.PLAN_SHA256,
            'Historical observation plan identity changed')
    samples = observations['samples']
    require(isinstance(samples, list) and len(samples) == len(previous.NAMES), 'Incomplete historical observations')
    result = {}
    for index, (sample, name, target) in enumerate(zip(samples, previous.NAMES, previous.TARGETS)):
        profile = dict(previous.PHYSICS, name=name, ordinary_output_target_dbm=target,
                       ordinary_role='reproduction_control' if index == 0 else 'new_prediction_check')
        previous._validate_sample(sample, profile)
        if target in HISTORICAL_PROFILES:
            require(name == HISTORICAL_PROFILES[target], 'Historical target identity changed')
            result[target] = sample
    require(set(result) == set(HISTORICAL_PROFILES), 'Incomplete historical target coverage')
    return result


def compare_baseline(plan, sample, prior_observations):
    profile = _registered_sample(plan, sample)
    require(profile['name'] in [plan['profiles'][index]['name'] for index in plan['historical_baseline_indices']],
            'Historical baseline requires a registered G20 zero')
    old = _historical_samples(prior_observations)[0]
    rows = [previous._comparison(sample['ordinary'], old['ordinary'], 'ordinary', 0,
                                 input_tolerance=POWER_TOLERANCE)]
    rows += previous._sweep_comparisons(sample, old, input_tolerance=POWER_TOLERANCE)
    return dict(classification='same_configuration_same_mode_complete_baseline', profile=profile['name'],
                reference_profile=old['profile']['name'], comparison_count=len(rows), rows=rows,
                all_comparisons_matched=all(row['matched'] for row in rows),
                full_historical_sweep_reproduced=all(row['matched'] for row in rows if row['category'] != 'ordinary'),
                vendor_compatibility_proven=False)


def compare_repeated_sweep(plan, sample, control_sample):
    profile = _registered_sample(plan, sample)
    control = _registered_sample(plan, control_sample)
    require(profile['name'] in [plan['profiles'][index]['name'] for index in plan['repeated_sweep_indices']],
            'Initial gain reference is not a repeated sweep')
    require(control['name'] == profile['control_profile'] and control['gain_db'] == profile['gain_db'],
            'Repeated sweep requires the registered same-gain control')
    rows = previous._sweep_comparisons(sample, control_sample, input_tolerance=INPUT_TOLERANCE)
    return dict(classification='same_gain_repeated_control_not_independent_prediction_checks',
                profile=profile['name'], reference_profile=control['name'], comparison_count=len(rows),
                all_comparisons_matched=all(row['matched'] for row in rows), rows=rows)


def score_point(profile, point):
    row = previous.score_point(profile, point)
    row['legacy_hypotheses'] = row.pop('hypotheses')
    q = Decimal(str(profile['ordinary_output_target_dbm']))
    pin = q - Decimal(str(profile['gain_db']))
    candidates = {}
    for name, coordinate, zero_set in ((HYPOTHESES[0], q, OUTPUT_ZERO_SET),
                                       (HYPOTHESES[1], pin, INPUT_ZERO_SET)):
        bypass = coordinate in zero_set
        increment = 0.0 if bypass else row['legacy_hypotheses']['raw_continuous']['predicted_increment_deg']
        predicted = row['source_phase_deg'] + increment
        error = previous.hyp.circular_error(row['observed_phase_deg'], predicted)
        candidates[name] = dict(nominal_coordinate_dbm=str(coordinate), bypass_applied=bypass,
                                power_axis='ordinary_raw_outside_registered_discrete_zero_set',
                                axis_output_dbm=row['raw_output_dbm'], predicted_increment_deg=increment,
                                predicted_phase_deg=predicted, circular_error_deg=error,
                                passed=error <= PHASE_TOLERANCE)
    row.update(hypotheses=candidates, gain_db=profile['gain_db'], nominal_source_dbm=str(pin),
               comparison_role=profile['comparison_role'], raw_input_power_w=point['raw_input_w'])
    return row


def _summary(rows, names, field):
    return {name: dict(point_count=len(rows), passed_count=sum(row[field][name]['passed'] for row in rows),
                       passed=bool(rows) and all(row[field][name]['passed'] for row in rows),
                       max_circular_error_deg=max((row[field][name]['circular_error_deg'] for row in rows), default=None))
            for name in names}


def _group(rows, expected_names):
    selected = [row for row in rows if row['profile'] in expected_names]
    present = {row['profile'] for row in selected}
    return dict(planned_count=len(expected_names), observed_count=len(selected),
                missing_count=len(expected_names) - len(selected),
                missing_profile_names=[name for name in expected_names if name not in present],
                complete=len(selected) == len(expected_names), summary=_summary(selected, HYPOTHESES, 'hypotheses'),
                legacy_summary=_summary(selected, LEGACY_HYPOTHESES, 'legacy_hypotheses'), rows=selected)


def _ordinary_comparison(new, old):
    comparison = previous._comparison(new, old, 'ordinary', 0, input_tolerance=REPLICATION_POWER_TOLERANCE)
    for power in comparison['powers'].values():
        power['relative_tolerance'] = REPLICATION_POWER_TOLERANCE
        power['matched'] = power['relative_error'] <= REPLICATION_POWER_TOLERANCE
    comparison['matched'] = all(item['matched'] for item in
                                list(comparison['powers'].values()) + list(comparison['phases'].values()))
    return comparison


def _allocation_pairs(plan, by_name, category):
    rows, missing = [], []
    pairs = plan['allocation_comparisons'][category]
    for left, right in pairs:
        if left not in by_name or right not in by_name:
            missing.append([left, right])
            continue
        first, second = by_name[left]['ordinary'], by_name[right]['ordinary']
        powers = {}
        for field in plan['allocation_comparisons']['exact_power_fields']:
            a, b = number(first[field]), number(second[field])
            powers[field] = dict(left_w=a, right_w=b, left_binary64_hex=a.hex(), right_binary64_hex=b.hex(),
                                 exact_binary64_equal=a.hex() == b.hex(), signed_difference_w=b - a,
                                 absolute_difference_w=abs(b - a), relative_error=relative(b, a),
                                 ratio=b / a, ratio_db=10 * math.log10(b / a))
        phases = previous._comparison(second, first, 'ordinary', 0,
                                      input_tolerance=REPLICATION_POWER_TOLERANCE)['phases']
        rows.append(dict(left_profile=left, right_profile=right, powers=powers, phases=phases,
                         zero_increment_states_equal=previous.score_point(by_name[left]['profile'], first)['zero_increment']
                         == previous.score_point(by_name[right]['profile'], second)['zero_increment']))
    return dict(planned_pair_count=len(pairs), completed_pair_count=len(rows), missing_pair_count=len(missing),
                missing_pairs=missing, complete=len(rows) == len(pairs), rows=rows, gates_acquisition=False,
                arbitrary_physical_power_function_refuted=False,
                interpretation=plan['allocation_comparisons']['interpretation'])


def _fingerprint(rows, role, planned_count):
    selected = [row for row in rows if row['role'] == 'new_prediction_check' and row['comparison_role'] == role]
    comparisons = []
    for row in selected:
        expected = Decimal(str(row['nominal_output_dbm'])) in OUTPUT_ZERO_SET if role == 'same_nominal_output' else True
        comparisons.append(dict(profile=row['profile'], predicted_zero_increment=expected,
                                observed_zero_increment=row['zero_increment'],
                                zero_increment_circular_error_deg=row['zero_increment_circular_error_deg'],
                                matched=row['zero_increment'] == expected))
    matched = bool(comparisons) and all(row['matched'] for row in comparisons)
    return dict(planned_applicable_count=planned_count, observed_applicable_count=len(comparisons),
                missing_applicable_count=planned_count - len(comparisons),
                all_observed_states_matched=matched,
                complete_fingerprint_matched=len(comparisons) == planned_count and matched,
                gates_acquisition=False, evaluates_only_zero_nonzero_state=True,
                reference='registered_historical_state_pattern_not_fitted_to_fresh_controls', rows=comparisons)


def score(plan, samples, *, allow_partial=False, historical_observations=None):
    profiles = validate_samples(plan, samples, allow_partial=allow_partial)
    by_name = {sample['profile']['name']: sample for sample in samples}
    rows = [score_point(profile, sample['ordinary']) for profile, sample in zip(profiles, samples)]
    primary = plan['primary_evaluation']
    groups = dict(new_prediction_checks=_group(rows, primary['new_prediction_profile_names']),
                  historical_reproductions=_group(rows, [p['name'] for p in profiles
                                                        if p['ordinary_role'] == 'historical_reproduction']),
                  divergent_prediction_checks=_group(rows, primary['divergent_profile_names']),
                  common_nonzero_prediction_checks=_group(rows, primary['common_nonzero_profile_names']))
    input_audit, raw_diagnostics, repeats = [], [], []
    for index, (profile, sample) in enumerate(zip(profiles, samples)):
        audit = _input_audit(profile, sample, by_name[profile['control_profile']], samples[0])
        input_audit.append(audit)
        point = sample['ordinary']
        raw_ratio = point['raw_input_w'] / samples[0]['ordinary']['raw_input_w']
        raw_gain = point['raw_output_w'] / point['raw_input_w']
        expected_gain = 10 ** (profile['gain_db'] / 10)
        raw_diagnostics.append(dict(profile=profile['name'], raw_input_w=point['raw_input_w'],
            dcp_input_power_w=point['input_power_w'],
            raw_vs_dcp_input_relative_error=relative(point['raw_input_w'], point['input_power_w']),
            global_control_profile=profiles[0]['name'], raw_input_ratio=raw_ratio,
            registered_input_ratio=audit['cross_gain']['registered_input_ratio'],
            raw_input_ratio_relative_error=relative(raw_ratio, audit['cross_gain']['registered_input_ratio']),
            raw_gain_linear=raw_gain, registered_gain_linear=expected_gain,
            raw_gain_relative_error=relative(raw_gain, expected_gain), raw_gain_db=10 * math.log10(raw_gain),
            raw_gain_error_db=10 * math.log10(raw_gain) - profile['gain_db'], gates_acquisition=False))
        comparison = compare_repeated_sweep(plan, sample, by_name[profile['control_profile']]) if (
            index in plan['repeated_sweep_indices']) else None
        repeats.append(dict(profile=profile['name'], gain_group=profile['gain_group'],
                            sweep=sample['sweep'], additional_sweep_points=sample['additional_sweep_points'],
                            initial_gain_reference=index in plan['gain_control_indices'],
                            new_auxiliary_reference=index in (3, 7), comparison=comparison))
    history_rows, baselines = [], []
    if historical_observations is not None:
        historical = _historical_samples(historical_observations)
        for index, (profile, sample) in enumerate(zip(profiles, samples)):
            if profile['ordinary_role'] == 'historical_reproduction':
                old = historical[profile['ordinary_output_target_dbm']]
                comparison = _ordinary_comparison(sample['ordinary'], old['ordinary'])
                comparison.update(profile=profile['name'], reference_profile=old['profile']['name'])
                history_rows.append(comparison)
            if index in plan['historical_baseline_indices']:
                baselines.append(compare_baseline(plan, sample, historical_observations))
    baseline_names = [profiles[index]['name'] for index in plan['historical_baseline_indices']]
    completed_baselines = {row['profile'] for row in baselines}
    history_match = bool(history_rows) and all(row['matched'] for row in history_rows)
    historical_result = dict(reference_provided=historical_observations is not None, planned_count=4,
        completed_count=len(history_rows), missing_count=4 - len(history_rows),
        all_completed_comparisons_matched=history_match,
        complete_historical_reproduction_matched=len(history_rows) == 4 and history_match,
        mismatches_are_results_not_acquisition_failures=True, rows=history_rows)
    repeat_rows = [row['comparison'] for row in repeats if row['comparison'] is not None]
    repeat_match = all(row['all_comparisons_matched'] for row in repeat_rows)
    baseline_match = bool(baselines) and all(row['all_comparisons_matched'] for row in baselines)
    complete = len(samples) == len(profiles)
    control_complete = complete and len(baselines) == 2 and baseline_match and len(repeat_rows) == 9 and repeat_match
    same_rows, same_missing = [], []
    for left, right in plan['same_configuration_pairs']:
        if left not in by_name or right not in by_name:
            same_missing.append([left, right])
        else:
            comparison = _ordinary_comparison(by_name[right]['ordinary'], by_name[left]['ordinary'])
            comparison.update(left_profile=left, right_profile=right)
            same_rows.append(comparison)
    return dict(schema_version=1, classification='finite_fixed_table_gain_source_allocation',
        sequence_status='complete' if complete else 'incomplete', all_planned_profiles_present=complete,
        planned_profile_count=12, observed_profile_count=len(samples), missing_profile_count=12 - len(samples),
        missing_profile_names=[p['name'] for p in profiles[len(samples):]],
        complete_acquisition_controls_passed=control_complete,
        interpretation_status='complete_finite_candidate_results' if control_complete else 'inconclusive',
        hypotheses=list(HYPOTHESES), hypothesis_definitions=dict(plan['hypothesis_definitions']),
        legacy_hypotheses=list(LEGACY_HYPOTHESES), historical_hypothesis_status=plan['historical_hypothesis_status'],
        **groups, input_identity_audit=input_audit, raw_input_and_gain_diagnostics=raw_diagnostics,
        allocation_comparisons={key: _allocation_pairs(plan, by_name, key) for key in
                                ('same_nominal_output_pairs', 'same_nominal_input_pairs')},
        finite_fingerprints=dict(output_pattern=_fingerprint(rows, 'same_nominal_output', 6),
                                 input_zero_pattern=_fingerprint(rows, 'same_nominal_input', 2)),
        historical_comparisons=historical_result,
        same_configuration_comparisons=dict(planned_pair_count=1, completed_pair_count=len(same_rows),
            missing_pair_count=len(same_missing), missing_pairs=same_missing, rows=same_rows, gates_acquisition=False),
        zero_baseline_controls=baselines, planned_zero_baseline_count=2, completed_zero_baseline_count=len(baselines),
        missing_zero_baseline_profiles=[name for name in baseline_names if name not in completed_baselines],
        all_observed_zero_baselines_matched=baseline_match, repeated_quick_sweeps=repeats,
        planned_repeated_sweep_count=9, completed_repeated_sweep_count=len(repeat_rows),
        missing_repeated_sweep_profiles=[profiles[index]['name'] for index in plan['repeated_sweep_indices']
                                         if index >= len(samples)],
        all_repeated_sweeps_matched=repeat_match, complete_repeated_sweep_collection=len(repeat_rows) == 9,
        planned_sweep_point_count=732, observed_sweep_point_count=sum(len(sample['sweep']) for sample in samples),
        additional_sweep_point_count=sum(len(sample['additional_sweep_points']) for sample in samples),
        additional_points_in_planned_scores=False, quick_sweeps_are_independent_prediction_checks=False,
        phase_absolute_tolerance_degrees=PHASE_TOLERANCE, parameters_fitted_this_study=0,
        eligible_for_vendor_compatibility=False, eligible_for_full_model_compatibility=False,
        raw_sources_verified=False, product_formula_changed=False, limitations=list(plan['exclusions']))


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
        complete_acquisition_controls_passed=result['complete_acquisition_controls_passed'],
        baseline_passed=result['all_observed_zero_baselines_matched'],
        repeated_sweeps_passed=result['all_repeated_sweeps_matched'])))
    return 0 if result['all_observed_zero_baselines_matched'] and result['all_repeated_sweeps_matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
