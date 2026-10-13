"""Score the frozen zero-dBm/first-knot discrimination without changing RFModel."""

import argparse
from decimal import Decimal
import hashlib
import importlib.util
import json
import math
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    'datapoints_hypotheses', Path(__file__).with_name('analyze-am-pm-datapoints.py'))
hyp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hyp)
number = hyp.number
PHASE_TOLERANCE = 1e-5
POWER_TOLERANCE = 1e-7
PLAN_SHA256 = '0ee1496412224c03daebb746db559435a2e4244b446180d913b67e2c8cd62af0'
HYPOTHESES = ('natural_cubic', 'nominal_zero_bypass', 'nominal_first_knot_bypass')
HYPOTHESIS_TEXT = [
    'Natural cubic spline on output dBm, numeric phase values, constant endpoint extension, added to measured source phase.',
    'As above with bypass at the registered nominal zero-dBm output, independent of table knot location. This is a finite discrimination rule, not a proposed numerical implementation of a vendor condition.',
    'As above with bypass at a registered nominal output coinciding with the first table knot, independent of absolute power.',
]
REFERENCES = {
    'ordinary_observations': {
        'path': 'validation/am-pm-datapoints-observations-20261012.json',
        'sha256': '66527c5c80264cfb2ba6394bd76754e5bcc65d5c81cc41309ef4980acdb3ba22'},
    'quick_sweep_evidence': {
        'path': 'validation/am-pm-existing-reference-evidence-20261012.json',
        'sha256': '7b1f1aca30b7098aea309e0e64916be8c2da7beac9b345a579d8bdfc7a5309a6'},
}
# The geometry is an independent guard against changing a frozen experiment and
# regenerating its predictions. Descriptions may grow without changing geometry.
PROFILE_SPECIFICATIONS = [
    ('original-triangle', [0, 10, 20], [10, 40, -20], 20, 0, 0, -25, 5, .5, 61),
    ('first-knot-negative', [-5, 5, 15], [10, 40, -20], 20, 37, 0, -30, 0, .5, 61),
    ('first-knot-positive', [5, 15, 25], [10, 40, -20], 20, -45, 5, -22.5, 7.5, .5, 61),
    ('zero-interior-knot', [-10, 0, 10], [15, 35, -25], 7, -29, 0, -22, 8, .5, 61),
    ('constant-phase-loss', [-5, 5, 15], [20, 20, 20], -3, 123, 0, -7, 23, .5, 61),
    ('unity-gain-control', [0, 10, 20], [10, 40, -20], 0, 63, 0, -5, 25, .5, 61),
    ('dense-zero-neighborhood', [0, 10, 20], [10, 40, -20], 20, 0, .001, -20.01, -19.99, .001, 21),
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def relative(actual, expected):
    return abs(number(actual) - number(expected)) / max(abs(actual), abs(expected), 1e-300)


def nominal_output(profile, index):
    return float(Decimal(str(profile['sweep_input_start_dbm']))
                 + Decimal(str(profile['sweep_step_db'])) * index
                 + Decimal(str(profile['gain_db'])))


def nominal_predictions(profile, target, source):
    spline = hyp.natural_cubic(profile['power_dbm'], profile['phase_deg'], target)
    return dict(natural_cubic=spline + source,
                nominal_zero_bypass=source if target == 0 else spline + source,
                nominal_first_knot_bypass=source if target == profile['power_dbm'][0] else spline + source)


def validate_plan(plan):
    require(type(plan['schema_version']) is int and plan['schema_version'] == 1, 'Wrong plan schema')
    require(number(plan['phase_absolute_tolerance_degrees']) == PHASE_TOLERANCE,
            'Registered phase tolerance changed')
    require(number(plan['baseline_power_relative_tolerance']) == POWER_TOLERANCE,
            'Registered power tolerance changed')
    require(plan['hypotheses'] == HYPOTHESIS_TEXT, 'Registered hypotheses changed')
    require(plan['source_phase_conventions'] == dict(
        ordinary='measured_input_node_phase', sweep='measured_input_node_phase',
        nominal_sweep_source_phase_degrees=0), 'Source phase ownership changed')
    number(plan['source_phase_conventions']['nominal_sweep_source_phase_degrees'])
    require(plan['analysis_target'] == 'RFModel_AMPM_A01/DataPoints/System2', 'Wrong analysis target')
    require(plan['source_workspace_sha256'] ==
            'ead90a212c786eff0cfa782dab47dbfe14c421b3f43b32c573bb990cb8348877', 'Wrong workspace source')
    require(plan['baseline_reference'] == REFERENCES, 'Baseline source identity changed')
    require(plan['prior_observations_sha256'] == REFERENCES['ordinary_observations']['sha256'],
            'Prior observation identity changed')
    require(plan['analysis_budget'] == dict(profiles=7, reserved_setup_recovery=1, total_maximum=8),
            'Analysis budget changed')
    require(all(type(value) is int for value in plan['analysis_budget'].values()), 'Noninteger budget')
    profiles = plan['profiles']
    require(isinstance(profiles, list) and len(profiles) == len(PROFILE_SPECIFICATIONS),
            'Exactly seven ordered profiles required')
    for profile, specification in zip(profiles, PROFILE_SPECIFICATIONS):
        name, powers, phases, gain, source, ordinary, start, stop, step, count = specification
        require(profile['name'] == name, 'Profile order/name changed')
        actual_powers, actual_phases = hyp.validate_table(profile['power_dbm'], profile['phase_deg'])
        require(actual_powers == powers and actual_phases == phases, 'Registered table changed: ' + name)
        expected = dict(gain_db=gain, source_phase_deg=source, ordinary_output_target_dbm=ordinary,
                        sweep_input_start_dbm=start, sweep_input_stop_dbm=stop, sweep_step_db=step,
                        frequency_hz=1e8, output_p1db_dbm=100, output_saturation_dbm=103,
                        output_ip2_dbm=140, output_ip3_dbm=130)
        for key, value in expected.items():
            require(number(profile[key]) == value, 'Registered parameter changed: ' + name + '/' + key)
        require(type(profile['expected_points']) is int and profile['expected_points'] == count,
                'Wrong planned point count')
        endpoint = float(Decimal(str(stop)) + Decimal(str(gain)))
        require(nominal_output(profile, count - 1) == endpoint, 'Inconsistent sweep endpoint')
        wanted = nominal_predictions(profile, ordinary, source)
        actual = profile['ordinary_nominal_predictions_degrees']
        require(set(actual) == set(HYPOTHESES), 'Incomplete ordinary predictions')
        for key in HYPOTHESES:
            require(abs(number(actual[key]) - wanted[key]) <= 1e-12, 'Changed ordinary prediction')
        predictions = profile['sweep_nominal_predictions']
        require(isinstance(predictions, list) and len(predictions) == count, 'Incomplete sweep predictions')
        for index, row in enumerate(predictions):
            require(set(row) == {'index', 'nominal_output_dbm', *HYPOTHESES}, 'Wrong sweep prediction fields')
            require(type(row['index']) is int and row['index'] == index, 'Changed prediction order')
            target = nominal_output(profile, index)
            require(number(row['nominal_output_dbm']) == target, 'Changed nominal prediction grid')
            for key, expected_value in nominal_predictions(profile, target, 0).items():
                require(abs(number(row[key]) - expected_value) <= 1e-12, 'Changed sweep prediction')
    return profiles


def validate_samples(plan, samples, *, allow_partial=False):
    profiles = validate_plan(plan)
    require(isinstance(samples, list) and 0 < len(samples) <= len(profiles), 'Empty or excessive observations')
    require(allow_partial or len(samples) == len(profiles), 'Incomplete profile collection')
    for sample, profile in zip(samples, profiles):
        require(json.dumps(sample['profile'], sort_keys=True, allow_nan=False)
                == json.dumps(profile, sort_keys=True, allow_nan=False), 'Observed profile differs from registration')
        sweep = sample['sweep']
        require(isinstance(sweep, list) and len(sweep) == profile['expected_points'], 'Incomplete planned sweep')
        extras = sample['additional_sweep_points']
        require(isinstance(extras, list) and len(extras) <= 1, 'Unexpected additional sweep points')
        points = [('ordinary', sample['ordinary'], profile['ordinary_output_target_dbm'])]
        for index, point in enumerate(sweep + extras):
            require(type(point['index']) is int and point['index'] == index, 'Sweep reordered or index changed')
            target = nominal_output(profile, index)
            require(number(point['nominal_output_dbm']) == target, 'Observed nominal grid changed')
            points.append(('sweep', point, target))
        for axis, point, target in points:
            input_power = number(point['input_power_w'])
            require(input_power > 0, 'Nonpositive measured input power')
            require(abs(10 * math.log10(input_power) + 30 - (target - profile['gain_db'])) <= 1e-5,
                    'Measured input does not represent registered input grid')
            source = number(point['input_phase_rad'])
            wanted = 0 if axis == 'sweep' else math.radians(profile['source_phase_deg'])
            require(abs(source - wanted) <= 1e-10, 'Ordinary/sweep source ownership changed')
            number(point['output_phase_rad'])
            require(number(point['output_power_w']) > 0, 'Nonpositive measured output power')
            if axis == 'ordinary':
                require(number(point['raw_output_w']) > 0, 'Nonpositive raw output power')
    return profiles


def score_point(profile, point, target, power_field):
    power = number(point[power_field])
    actual_dbm = 10 * math.log10(power) + 30
    source = math.degrees(number(point['input_phase_rad']))
    actual = math.degrees(number(point['output_phase_rad']))
    spline = hyp.natural_cubic(profile['power_dbm'], profile['phase_deg'], actual_dbm)
    candidates = {}
    for name in HYPOTHESES:
        bypass = ((name == 'nominal_zero_bypass' and target == 0)
                  or (name == 'nominal_first_knot_bypass' and target == profile['power_dbm'][0]))
        increment = 0 if bypass else spline
        predicted = source + increment
        error = hyp.circular_error(actual, predicted)
        candidates[name] = dict(predicted_phase_deg=predicted, predicted_increment_deg=increment,
                                bypass_at_registered_nominal_target=bypass,
                                circular_error_deg=error, passed=error <= PHASE_TOLERANCE)
    return dict(profile=profile['name'], index=point.get('index', 0), nominal_output_dbm=target,
                output_power_w=power, power_axis_field=power_field, axis_output_dbm=actual_dbm,
                source_phase_deg=source, observed_phase_deg=actual,
                observed_increment_deg=actual - source, hypotheses=candidates)


def summarize(rows):
    return {name: dict(point_count=len(rows),
                       passed_count=sum(row['hypotheses'][name]['passed'] for row in rows),
                       passed=bool(rows) and all(row['hypotheses'][name]['passed'] for row in rows),
                       max_circular_error_deg=max((row['hypotheses'][name]['circular_error_deg']
                                                  for row in rows), default=None)) for name in HYPOTHESES}


def score(plan, samples, *, allow_partial=False):
    profiles = validate_samples(plan, samples, allow_partial=allow_partial)
    rows = {'sweep': [], 'ordinary_report': [], 'ordinary_raw': []}
    extra_rows = []
    for sample, profile in zip(samples, profiles):
        for axis in ('ordinary_report', 'ordinary_raw'):
            field = 'raw_output_w' if axis == 'ordinary_raw' else 'output_power_w'
            rows[axis].append(score_point(profile, sample['ordinary'], profile['ordinary_output_target_dbm'], field))
        rows['sweep'].extend(score_point(profile, point, nominal_output(profile, point['index']), 'output_power_w')
                             for point in sample['sweep'])
        extra_rows.extend(score_point(profile, point, nominal_output(profile, point['index']), 'output_power_w')
                          for point in sample['additional_sweep_points'])
    axes = {name: dict(summary=summarize(values), rows=values,
                       profiles={profile['name']: summarize([r for r in values if r['profile'] == profile['name']])
                                 for profile in profiles[:len(samples)]}) for name, values in rows.items()}
    return dict(schema_version=1, classification='finite_zero_dbm_first_knot_discrimination_not_vendor_contract',
                all_planned_profiles_present=len(samples) == len(profiles),
                planned_sweep_point_count=sum(profile['expected_points'] for profile in profiles),
                observed_sweep_point_count=len(rows['sweep']), phase_absolute_tolerance_degrees=PHASE_TOLERANCE,
                hypotheses=list(HYPOTHESES), axes=axes, additional_sweep_points=extra_rows,
                additional_points_in_planned_scores=False, parameters_fitted_this_study=0,
                eligible_for_vendor_compatibility=False, raw_sources_verified=False,
                product_formula_changed=False,
                limitations=['The exception location uses registered nominal indices, never rounded DCP power.',
                             'Power-axis residuals use measured DCP or separately identified ordinary raw power.',
                             'Quick Sweep raw carrier phasors are unavailable; ordinary phasors do not replace them.',
                             'Finite hypotheses do not uniquely identify an arbitrary-table vendor algorithm.'])


def compare_baseline(plan, new_sample, prior_observations, prior_quick_evidence):
    validate_samples(plan, [new_sample], allow_partial=True)
    ordinary_prior = [sample for sample in prior_observations['samples']
                      if sample['profile'] == 'interpolation-boundary']
    old_by_target = {}
    for sample in ordinary_prior:
        target = number(sample['target_output_dbm'])
        require(target not in old_by_target, 'Duplicate prior ordinary target')
        old_by_target[target] = sample
    new_by_target = {point['nominal_output_dbm']: point for point in new_sample['sweep']}
    quick_prior = [sample for sample in prior_quick_evidence['quick_sweeps']
                   if sample['profile']['name'] == 'natural-cubic-followup']
    require(len(quick_prior) == 1, 'Missing or duplicate historical Quick Sweep')
    rows = []

    def compare(category, axis, target, new, old_power, old_phase, old_source):
        power = number(new['raw_output_w' if axis == 'ordinary_raw' else 'output_power_w'])
        require(power > 0 and number(old_power) > 0, 'Invalid baseline power')
        phase = math.degrees(number(new['output_phase_rad']) - number(new['input_phase_rad']))
        power_error = relative(power, old_power)
        phase_error = hyp.circular_error(phase, number(old_phase) - number(old_source))
        rows.append(dict(category=category, axis=axis, nominal_output_dbm=target,
                         new_output_power_w=power, prior_output_power_w=old_power,
                         power_relative_error=power_error, new_increment_deg=phase,
                         prior_increment_deg=old_phase - old_source, phase_circular_error_deg=phase_error,
                         matched=power_error <= POWER_TOLERANCE and phase_error <= PHASE_TOLERANCE))

    require(0 in old_by_target, 'Missing prior ordinary zero target')
    original = old_by_target[0]
    for axis, field in [('ordinary_report', 'measured_output_w'), ('ordinary_raw', 'raw_output_w')]:
        compare('same_mode_ordinary_reproduction', axis, 0, new_sample['ordinary'], original[field],
                original['measured_output_phase_deg'], original['measured_source_phase_deg'])
    for target in (-5, 0, 5, 10, 15, 20, 25):
        require(target in old_by_target and target in new_by_target, 'Missing shared ordinary/sweep point')
        old = old_by_target[target]
        compare('cross_mode_sweep_to_ordinary', 'sweep_report', target, new_by_target[target],
                old['measured_output_w'], old['measured_output_phase_deg'], old['measured_source_phase_deg'])
    for target in (0, 10, 20):
        prior_points = [point for point in quick_prior[0]['points']
                        if number(point['swept_input_w']) > 0
                        and abs(10 * math.log10(point['swept_input_w']) + 50 - target) <= 1e-9]
        require(len(prior_points) == 1, 'Missing or ambiguous historical Quick Sweep target')
        old = prior_points[0]
        compare('same_path_different_sweep_mode', 'sweep_report', target, new_by_target[target],
                old['reported_output_w'], math.degrees(number(old['output_phase_rad'])),
                math.degrees(number(old['input_phase_rad'])))
    return dict(classification='ordinary_reproduction_and_separate_cross_mode_controls',
                phase_absolute_tolerance_degrees=PHASE_TOLERANCE,
                power_relative_tolerance=POWER_TOLERANCE, comparison_count=len(rows),
                all_comparisons_matched=all(row['matched'] for row in rows), rows=rows,
                categories={category: dict(count=sum(row['category'] == category for row in rows),
                                            matched=all(row['matched'] for row in rows if row['category'] == category))
                            for category in dict.fromkeys(row['category'] for row in rows)},
                full_historical_sweep_reproduced=False, vendor_compatibility_proven=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    require(not args.output.exists(), 'Output exists')
    require(digest(args.plan) == PLAN_SHA256, 'Frozen plan bytes changed')
    plan, evidence = load(args.plan), load(args.evidence)
    require(evidence['plan_sha256'] == digest(args.plan), 'Evidence references different plan bytes')
    result = score(plan, evidence['samples'])
    references = {}
    for name, source in REFERENCES.items():
        path = args.root / source['path']
        require(digest(path) == source['sha256'], 'Prior baseline bytes changed')
        references[name] = load(path)
    result['baseline'] = compare_baseline(plan, evidence['samples'][0], references['ordinary_observations'],
                                          references['quick_sweep_evidence'])
    result.update(plan_sha256=digest(args.plan), evidence_sha256=digest(args.evidence),
                  baseline_file_hashes_verified=True)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(dict(baseline_passed=result['baseline']['all_comparisons_matched'],
                          sweep=result['axes']['sweep']['summary'])))
    return 0 if result['baseline']['all_comparisons_matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
