"""Score the frozen ordinary raw-axis study without running SystemVue or fitting data."""

import argparse
from decimal import Decimal
import hashlib
import importlib.util
import json
import math
from pathlib import Path


def _module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


hyp = _module('raw_axis_hypotheses', 'analyze-am-pm-datapoints.py')
normalizer = _module('raw_axis_normalizer', 'archive-am-pm-datapoints-zero-study.py')
number = normalizer.number
PHASE_TOLERANCE = 1e-5
POWER_TOLERANCE = 1e-7
INPUT_TOLERANCE = 1e-10
PLAN_SHA256 = 'd6d2624f410b35a458988e82bb619b1548f67fd3612adc555b569842d9b125f7'
HYPOTHESES = ('dcp_continuous', 'raw_continuous', 'raw_nominal_zero_bypass', 'raw_report_zero_bypass')
REFERENCE = dict(path='validation/am-pm-datapoints-zero-observations-20261013.json',
                 sha256='77a8712efd6e000cc70fe93ce6d586adae803c35674cb334f4de8cb0d518fef7')
TARGETS = [0, -.005, -.0045, -.004, -.0005, -.000001, .000001, .0005, .004, .0045, .005]
NAMES = ['control-zero', 'negative-0p005', 'negative-0p0045', 'negative-0p004',
         'negative-0p0005', 'negative-0p000001', 'positive-0p000001',
         'positive-0p0005', 'positive-0p004', 'positive-0p0045', 'positive-0p005']
PHYSICS = dict(power_dbm=[-5, 5, 15], phase_deg=[10, 40, -20], gain_db=20,
               frequency_hz=1e8, source_phase_deg=37, output_p1db_dbm=100,
               output_saturation_dbm=103, output_ip2_dbm=140, output_ip3_dbm=130,
               sweep_input_start_dbm=-30, sweep_input_stop_dbm=0, sweep_step_db=.5,
               expected_points=61)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def relative(actual, expected):
    actual, expected = number(actual), number(expected)
    return abs(actual - expected) / max(abs(actual), abs(expected), 1e-300)


def identical(actual, expected):
    return json.dumps(actual, sort_keys=True, allow_nan=False) == json.dumps(expected, sort_keys=True, allow_nan=False)


def nominal_output(profile, index):
    return float(Decimal(str(profile['sweep_input_start_dbm']))
                 + Decimal(str(profile['sweep_step_db'])) * index + Decimal(str(profile['gain_db'])))


def validate_plan(plan):
    require(type(plan['schema_version']) is int and plan['schema_version'] == 1, 'Wrong plan schema')
    for key, expected in [('phase_absolute_tolerance_degrees', PHASE_TOLERANCE),
                          ('baseline_power_relative_tolerance', POWER_TOLERANCE),
                          ('input_power_relative_tolerance', INPUT_TOLERANCE)]:
        require(number(plan[key]) == expected, 'Registered tolerance changed: ' + key)
    require(plan['hypotheses'] == list(HYPOTHESES), 'Registered hypotheses changed')
    require(plan['analysis_target'] == normalizer.ANALYSIS, 'Wrong analysis target')
    require(plan['source_workspace_sha256'] ==
            'ead90a212c786eff0cfa782dab47dbfe14c421b3f43b32c573bb990cb8348877', 'Wrong source workspace')
    require(plan['baseline_reference'] == REFERENCE and plan['baseline_profile'] == 'first-knot-negative',
            'Historical control identity changed')
    require(identical(plan['analysis_budget'], dict(profiles=11, reserved_setup_recovery=1, total_maximum=12)),
            'Analysis budget changed')
    require(plan['source_phase_conventions'] == dict(ordinary='measured_input_node_phase',
            sweep='measured_input_node_phase', nominal_sweep_source_phase_degrees=0), 'Source phase ownership changed')
    require(plan['local_spline_polynomial']['domain_dbm'] == [-5, 5]
            and plan['local_spline_polynomial']['ascending_coefficients'] == [33.4375, 3.5625, -.3375, -.0225],
            'Analytic spline changed')
    primary = plan['primary_evaluation']
    require(type(primary['control_count']) is int and primary['control_count'] == 1
            and type(primary['new_prediction_check_count']) is int and primary['new_prediction_check_count'] == 10
            and primary['source_and_report_axes_separate'] is True
            and primary['zero_control_excluded_from_new_prediction_pass_rates'] is True, 'Primary/control scope changed')
    quick = plan['quick_sweeps']
    for key, expected in [('planned_points_per_call', 61), ('maximum_extra_endpoints_per_call', 1),
                          ('total_planned_observations', 671)]:
        require(type(quick[key]) is int and quick[key] == expected, 'Repeated-sweep budget changed')
    require(quick['required_parameter_readback'] == normalizer.SWEEP + 'Enable == 1', 'Sweep enable guard changed')
    require(isinstance(quick['repeated_sweep_gate'], str) and bool(quick['repeated_sweep_gate']),
            'Missing repeated-sweep gate')
    require(set(plan['input_power_identity']) == {'ordinary', 'repeated_sweep', 'source_parameter', 'baseline_input'}
            and all(isinstance(v, str) and v for v in plan['input_power_identity'].values()), 'Missing input identity rules')
    profiles = plan['profiles']
    require(isinstance(profiles, list) and len(profiles) == 11, 'Exactly eleven ordered profiles required')
    for index, (profile, name, target) in enumerate(zip(profiles, NAMES, TARGETS)):
        expected = dict(PHYSICS, name=name, ordinary_output_target_dbm=target,
                        ordinary_role='reproduction_control' if index == 0 else 'new_prediction_check')
        require(set(profile) == set(expected), 'Registered profile fields changed')
        for key, value in expected.items():
            actual = profile[key]
            if isinstance(value, list):
                require(isinstance(actual, list) and [number(v) for v in actual] == value, 'Registered table changed')
            elif isinstance(value, str):
                require(actual == value, 'Profile order/role changed')
            else:
                require(number(actual) == value, 'Registered parameter changed: ' + key)
        require(type(profile['expected_points']) is int, 'Invalid sweep count')
    return profiles


def _validate_sample(sample, profile):
    require(identical(sample['profile'], profile), 'Observed profile differs from registration')
    require(sample['analysis_target'] == normalizer.ANALYSIS, 'Observed analysis target changed')
    expected = normalizer.expected_parameters(profile)
    for key, wanted in expected.items():
        actual = sample['parameters_si'][key]
        pairs = zip(actual, wanted) if isinstance(wanted, list) else [(actual, wanted)]
        if isinstance(wanted, list):
            require(isinstance(actual, list) and len(actual) == len(wanted), 'Parameter array shape changed')
        require(all(abs(number(a) - b) <= 1e-12 * max(1e-12, abs(b)) for a, b in pairs),
                'Run-time parameter mismatch: ' + key)
    sweep, extras = sample['sweep'], sample['additional_sweep_points']
    require(isinstance(sweep, list) and len(sweep) == 61, 'Incomplete planned sweep')
    require(isinstance(extras, list) and len(extras) <= 1, 'Unexpected additional sweep points')
    ordinary = sample['ordinary']
    require(type(ordinary['index']) is int and ordinary['index'] == 0, 'Ordinary index changed')
    points = [('ordinary', ordinary, profile['ordinary_output_target_dbm'])]
    for index, point in enumerate(sweep + extras):
        require(type(point['index']) is int and point['index'] == index, 'Sweep index/order changed')
        target = nominal_output(profile, index)
        require(number(point['nominal_output_dbm']) == target, 'Observed nominal sweep grid changed')
        points.append(('sweep', point, target))
    for mode, point, target in points:
        pin = number(point['input_power_w'])
        require(pin > 0 and number(point['output_power_w']) > 0, 'Nonpositive measured power')
        require(abs(10 * math.log10(pin) + 30 - (target - profile['gain_db'])) <= 1e-5,
                'Measured input does not represent registered grid')
        source = number(point['input_phase_rad'])
        require(abs(source - (math.radians(profile['source_phase_deg']) if mode == 'ordinary' else 0)) <= 1e-10,
                'Ordinary/sweep source ownership changed')
        number(point['output_phase_rad'])
    raw = number(ordinary['raw_output_w'])
    require(raw > 0, 'Nonpositive raw output power')
    carrier = ordinary['raw_carrier']
    require(isinstance(carrier, list) and len(carrier) == 2, 'Missing raw carrier pair')
    require(sorted(number(c['frequency_hz']) for c in carrier) == [1e8 - .5, 1e8 + .5],
            'Unexpected raw carrier support')
    for item in carrier:
        require(isinstance(item['voltage'], list) and len(item['voltage']) == 2
                and isinstance(item['impedance'], list) and len(item['impedance']) == 2, 'Invalid phasor shape')
        voltage = complex(*map(number, item['voltage']))
        impedance = complex(*map(number, item['impedance']))
        require(impedance != 0 and number(item['power_w']) > 0, 'Invalid raw power/impedance')
        physical = .5 * abs(voltage) ** 2 * (1 / impedance).real
        require(physical > 0 and relative(physical, item['power_w']) <= 1e-12, 'Raw V/Z/P inconsistency')
        phase = math.degrees(math.atan2(voltage.imag, voltage.real))
        require(hyp.circular_error(phase, number(item['output_phase_deg'])) <= 1e-8
                and hyp.circular_error(phase, math.degrees(ordinary['output_phase_rad'])) <= 1e-8,
                'Raw versus ordinary phase mismatch')
        require(isinstance(item['label'], str) and item['label'].startswith('{1}D[')
                and item['label'].endswith(',Source,RFAmp'), 'Wrong raw carrier identity')
    require(carrier[0]['label'] == carrier[1]['label'], 'Inconsistent raw carrier identity')
    require(relative(carrier[0]['power_w'], carrier[1]['power_w']) <= 1e-12, 'Nonconstant raw carrier power')
    require(relative(raw, carrier[0]['power_w'] / 2 + carrier[1]['power_w'] / 2) <= 1e-12,
            'Raw mean power mismatch')
    require(abs(number(ordinary['raw_vs_report_relative_error'])
                - relative(raw, ordinary['output_power_w'])) <= 1e-15, 'Raw/report residual changed')


def validate_samples(plan, samples, *, allow_partial=False):
    profiles = validate_plan(plan)
    require(isinstance(samples, list) and 0 < len(samples) <= len(profiles), 'Empty or excessive observations')
    require(allow_partial or len(samples) == len(profiles), 'Incomplete profile collection')
    for sample, profile in zip(samples, profiles):
        _validate_sample(sample, profile)
        ratio = sample['ordinary']['input_power_w'] / samples[0]['ordinary']['input_power_w']
        expected_ratio = 10 ** (profile['ordinary_output_target_dbm'] / 10)
        require(relative(ratio, expected_ratio) <= INPUT_TOLERANCE, 'Ordinary registered input-power ratio mismatch')
    return profiles


def _comparison(new, old, category, index, *, input_tolerance):
    power_fields = ['input_power_w', 'output_power_w']
    if category == 'ordinary':
        power_fields.append('raw_output_w')
    powers = {}
    for field in power_fields:
        require(number(new[field]) > 0 and number(old[field]) > 0, 'Nonpositive comparison power')
        tolerance = input_tolerance if field == 'input_power_w' else POWER_TOLERANCE
        error = relative(new[field], old[field])
        powers[field] = dict(new_w=new[field], reference_w=old[field], relative_error=error,
                             relative_tolerance=tolerance, matched=error <= tolerance)
    phase_errors = {}
    for field in ['input_phase_rad', 'output_phase_rad']:
        error = hyp.circular_error(math.degrees(number(new[field])), math.degrees(number(old[field])))
        phase_errors[field] = dict(circular_error_deg=error, matched=error <= PHASE_TOLERANCE)
    increment = math.degrees(new['output_phase_rad'] - new['input_phase_rad'])
    reference_increment = math.degrees(old['output_phase_rad'] - old['input_phase_rad'])
    error = hyp.circular_error(increment, reference_increment)
    phase_errors['increment'] = dict(new_deg=increment, reference_deg=reference_increment,
                                    circular_error_deg=error, matched=error <= PHASE_TOLERANCE)
    return dict(category=category, index=index, powers=powers, phases=phase_errors,
                matched=all(item['matched'] for item in list(powers.values()) + list(phase_errors.values())))


def _sweep_comparisons(new, old, *, input_tolerance):
    rows = []
    for field, category in [('sweep', 'planned_sweep'), ('additional_sweep_points', 'additional_sweep')]:
        require(len(new[field]) == len(old[field]), 'Control sweep/extra length mismatch')
        for left, right in zip(new[field], old[field]):
            require(left['index'] == right['index'] and left['nominal_output_dbm'] == right['nominal_output_dbm'],
                    'Control sweep indices/targets differ')
            rows.append(_comparison(left, right, category, left['index'], input_tolerance=input_tolerance))
    return rows


def compare_baseline(plan, new_sample, prior_observations):
    validate_samples(plan, [new_sample], allow_partial=True)
    prior = [sample for sample in prior_observations['samples'] if sample['profile']['name'] == plan['baseline_profile']]
    require(len(prior) == 1, 'Missing or duplicate historical control')
    old = prior[0]
    for key, value in PHYSICS.items():
        require(old['profile'][key] == value, 'Historical control physical configuration changed')
    require(old['profile']['ordinary_output_target_dbm'] == 0, 'Historical ordinary target changed')
    rows = [_comparison(new_sample['ordinary'], old['ordinary'], 'ordinary', 0, input_tolerance=POWER_TOLERANCE)]
    rows += _sweep_comparisons(new_sample, old, input_tolerance=POWER_TOLERANCE)
    return dict(classification='same_configuration_same_mode_complete_baseline', comparison_count=len(rows),
                all_comparisons_matched=all(row['matched'] for row in rows), rows=rows,
                full_historical_sweep_reproduced=all(row['matched'] for row in rows if row['category'] != 'ordinary'),
                vendor_compatibility_proven=False)


def compare_repeated_sweep(plan, new_sample, control_sample):
    profiles = validate_plan(plan)
    _validate_sample(control_sample, profiles[0])
    matches = [profile for profile in profiles if profile['name'] == new_sample['profile']['name']]
    require(len(matches) == 1, 'Unregistered repeat profile')
    _validate_sample(new_sample, matches[0])
    rows = _sweep_comparisons(new_sample, control_sample, input_tolerance=INPUT_TOLERANCE)
    return dict(classification='repeated_control_sweep_not_independent_prediction_checks',
                profile=new_sample['profile']['name'], reference_profile=control_sample['profile']['name'],
                comparison_count=len(rows), all_comparisons_matched=all(row['matched'] for row in rows), rows=rows)


def score_point(profile, point):
    target = profile['ordinary_output_target_dbm']
    source, observed = math.degrees(point['input_phase_rad']), math.degrees(point['output_phase_rad'])
    dcp_dbm = 10 * math.log10(point['output_power_w']) + 30
    raw_dbm = 10 * math.log10(point['raw_output_w']) + 30
    candidates = {}
    for name in HYPOTHESES:
        axis = dcp_dbm if name == 'dcp_continuous' else raw_dbm
        bypass = (name == 'raw_nominal_zero_bypass' and target == 0
                  or name == 'raw_report_zero_bypass' and point['output_power_w'] == .001)
        increment = 0 if bypass else hyp.natural_cubic(profile['power_dbm'], profile['phase_deg'], axis)
        predicted = source + increment
        error = hyp.circular_error(observed, predicted)
        candidates[name] = dict(power_axis='ordinary_dcp' if name == 'dcp_continuous' else 'ordinary_raw',
                                axis_output_dbm=axis, bypass_applied=bypass, predicted_increment_deg=increment,
                                predicted_phase_deg=predicted, circular_error_deg=error, passed=error <= PHASE_TOLERANCE)
    zero_error = hyp.circular_error(observed, source)
    return dict(profile=profile['name'], role=profile['ordinary_role'], nominal_output_dbm=target,
                input_power_w=point['input_power_w'], dcp_output_power_w=point['output_power_w'],
                raw_output_power_w=point['raw_output_w'], dcp_output_dbm=dcp_dbm, raw_output_dbm=raw_dbm,
                raw_vs_report_relative_error=relative(point['raw_output_w'], point['output_power_w']),
                source_phase_deg=source, observed_phase_deg=observed, observed_increment_deg=observed - source,
                zero_increment_circular_error_deg=zero_error, zero_increment=zero_error <= PHASE_TOLERANCE,
                hypotheses=candidates)


def summarize(rows):
    return {name: dict(point_count=len(rows), passed_count=sum(row['hypotheses'][name]['passed'] for row in rows),
                       passed=bool(rows) and all(row['hypotheses'][name]['passed'] for row in rows),
                       max_circular_error_deg=max((row['hypotheses'][name]['circular_error_deg'] for row in rows), default=None))
            for name in HYPOTHESES}


def score(plan, samples, *, allow_partial=False):
    profiles = validate_samples(plan, samples, allow_partial=allow_partial)
    rows = [score_point(profile, sample['ordinary']) for profile, sample in zip(profiles, samples)]
    repeats = [dict(profile=sample['profile']['name'], sweep=sample['sweep'],
                    additional_sweep_points=sample['additional_sweep_points'],
                    comparison=compare_repeated_sweep(plan, sample, samples[0]) if index else None)
               for index, sample in enumerate(samples)]
    return dict(schema_version=1, classification='finite_ordinary_raw_axis_zero_bypass_discrimination',
                all_planned_profiles_present=len(samples) == len(profiles),
                planned_new_prediction_check_count=10, observed_new_prediction_check_count=len(rows) - 1,
                planned_sweep_point_count=671, observed_sweep_point_count=61 * len(samples),
                phase_absolute_tolerance_degrees=PHASE_TOLERANCE, hypotheses=list(HYPOTHESES),
                new_prediction_checks=dict(summary=summarize(rows[1:]), rows=rows[1:]),
                reproduction_controls=dict(summary=summarize(rows[:1]), rows=rows[:1]),
                repeated_quick_sweeps=repeats,
                all_repeated_sweeps_matched=all(item['comparison']['all_comparisons_matched'] for item in repeats[1:]),
                additional_points_in_planned_scores=False, quick_sweeps_are_independent_prediction_checks=False,
                parameters_fitted_this_study=0, eligible_for_vendor_compatibility=False,
                raw_sources_verified=False, product_formula_changed=False,
                limitations=['The zero control is excluded from ten new prediction pass rates.',
                             'Quick Sweep observations are repeated controls; swept raw spectra are unavailable.',
                             'The +/-1e-6 dBm points distinguish zero from nonzero increment, not the exact internal axis.',
                             'Finite points do not establish exact bypass/reporting boundaries or a vendor contract.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--raw-root', type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output exists')
    require(args.raw_root is None, 'Raw provenance verification requires acquisition-wrapper integration')
    require(digest(args.plan) == PLAN_SHA256, 'Frozen plan bytes changed')
    plan, evidence = load(args.plan), load(args.evidence)
    require(evidence['plan_sha256'] == PLAN_SHA256, 'Evidence references different plan bytes')
    result = score(plan, evidence['samples'])
    reference = args.root / REFERENCE['path']
    require(digest(reference) == REFERENCE['sha256'], 'Prior baseline bytes changed')
    result['baseline'] = compare_baseline(plan, evidence['samples'][0], load(reference))
    result.update(plan_sha256=PLAN_SHA256, evidence_sha256=digest(args.evidence), baseline_file_hashes_verified=True)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(dict(baseline_passed=result['baseline']['all_comparisons_matched'],
                          repeated_sweeps_passed=result['all_repeated_sweeps_matched'],
                          new_prediction_checks=result['new_prediction_checks']['summary'])))
    return 0 if result['baseline']['all_comparisons_matched'] and result['all_repeated_sweeps_matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
