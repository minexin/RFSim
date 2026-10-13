"""Independent synthetic discrimination, fixed-plan guards, and baseline controls."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('diagnose-am-pm-datapoints-zero-study.py')
spec = importlib.util.spec_from_file_location('zero_study', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
ROOT = SCRIPT.resolve().parents[2]
PLAN_PATH = ROOT / 'validation/am-pm-datapoints-zero-plan-20261013.json'


def independent_spline(profile, q):
    """Closed form for the registered equally spaced three-knot tables."""
    x, y = profile['power_dbm'], profile['phase_deg']
    if q <= x[0]:
        return y[0]
    if q >= x[2]:
        return y[2]
    h = x[1] - x[0]
    assert h == x[2] - x[1] == 10
    second = 1.5 * (y[2] - 2 * y[1] + y[0]) / (h * h)
    if q <= x[1]:
        t, endpoint, adjacent = q - x[0], y[0], y[1]
    else:
        t, endpoint, adjacent = x[2] - q, y[2], y[1]
    return endpoint + ((adjacent - endpoint) / h - second * h / 6) * t + second * t**3 / (6 * h)


def synthetic_samples(plan, hypothesis='nominal_zero_bypass', extras=False):
    samples = []
    for profile in plan['profiles']:
        def point(target, source, index):
            phase = independent_spline(profile, target)
            if ((hypothesis == 'nominal_zero_bypass' and target == 0)
                    or (hypothesis == 'nominal_first_knot_bypass' and target == profile['power_dbm'][0])):
                phase = 0
            return dict(index=index, nominal_output_dbm=target,
                        input_power_w=10**((target - profile['gain_db'] - 30) / 10),
                        output_power_w=10**((target - 30) / 10),
                        input_phase_rad=math.radians(source), output_phase_rad=math.radians(source + phase))
        ordinary = point(profile['ordinary_output_target_dbm'], profile['source_phase_deg'], 0)
        ordinary['raw_output_w'] = ordinary['output_power_w']
        # Round decimal input arithmetic independently of the scorer's Decimal grid.
        def target_at(index):
            return round(profile['sweep_input_start_dbm'] + index * profile['sweep_step_db'] + profile['gain_db'], 12)
        sweep = [point(target_at(i), 0, i) for i in range(profile['expected_points'])]
        additional = [point(target_at(profile['expected_points']), 0, profile['expected_points'])] if extras else []
        samples.append(dict(profile=copy.deepcopy(profile), ordinary=ordinary, sweep=sweep,
                            additional_sweep_points=additional))
    return samples


class DataPointsZeroStudyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = m.load(PLAN_PATH)
        cls.prior = m.load(ROOT / m.REFERENCES['ordinary_observations']['path'])
        cls.quick = m.load(ROOT / m.REFERENCES['quick_sweep_evidence']['path'])

    def samples(self, hypothesis='nominal_zero_bypass', extras=False):
        return synthetic_samples(self.plan, hypothesis, extras)

    def baseline_samples(self):
        samples = self.samples()
        first = samples[0]
        old = {row['target_output_dbm']: row for row in self.prior['samples']
               if row['profile'] == 'interpolation-boundary'}
        for point in first['sweep']:
            target = point['nominal_output_dbm']
            if target in old:
                point['output_power_w'] = old[target]['measured_output_w']
                point['output_phase_rad'] = math.radians(old[target]['measured_output_phase_deg'])
        first['ordinary']['output_power_w'] = old[0]['measured_output_w']
        first['ordinary']['output_phase_rad'] = math.radians(old[0]['measured_output_phase_deg'])
        first['ordinary']['raw_output_w'] = old[0]['raw_output_w']
        return samples

    def test_frozen_plan_and_prior_bytes(self):
        self.assertEqual(m.digest(PLAN_PATH), m.PLAN_SHA256)
        for reference in m.REFERENCES.values():
            self.assertEqual(m.digest(ROOT / reference['path']), reference['sha256'])
        self.assertEqual(len(m.validate_plan(self.plan)), 7)
        self.assertEqual(sum(p['expected_points'] for p in self.plan['profiles']), 387)

    def test_independent_spline_known_values_and_shifted_first_knot(self):
        original, shifted = self.plan['profiles'][0], self.plan['profiles'][2]
        self.assertEqual(independent_spline(original, 5), 33.4375)
        self.assertEqual(independent_spline(original, 15), 18.4375)
        self.assertEqual(independent_spline(original, 2.5), 22.7734375)
        self.assertAlmostEqual(independent_spline(original, .001), 10.0052499999775)
        self.assertEqual(shifted['ordinary_output_target_dbm'], 5)
        self.assertEqual(shifted['ordinary_nominal_predictions_degrees'], dict(
            natural_cubic=-35.0, nominal_zero_bypass=-35.0, nominal_first_knot_bypass=-45))
        self.assertEqual(shifted['sweep_nominal_predictions'][15]['nominal_output_dbm'], 5)
        self.assertEqual(shifted['sweep_nominal_predictions'][5]['nominal_output_dbm'], 0)

    def test_each_hypothesis_is_distinguished_by_independent_complete_data(self):
        for name in m.HYPOTHESES:
            with self.subTest(hypothesis=name):
                result = m.score(self.plan, self.samples(name))
                self.assertTrue(result['all_planned_profiles_present'])
                self.assertEqual(result['observed_sweep_point_count'], 387)
                for axis, count in [('sweep', 387), ('ordinary_report', 7), ('ordinary_raw', 7)]:
                    summary = result['axes'][axis]['summary']
                    self.assertEqual([key for key, value in summary.items() if value['passed']], [name])
                    self.assertEqual(summary[name]['passed_count'], count)
                self.assertFalse(result['eligible_for_vendor_compatibility'])
                self.assertFalse(result['raw_sources_verified'])
                self.assertFalse(result['product_formula_changed'])
                self.assertEqual(result['parameters_fitted_this_study'], 0)

    def test_measured_power_is_used_even_away_from_nominal_grid(self):
        samples = self.samples()
        point = samples[0]['sweep'][20]  # Nominal output 5 dBm.
        point['output_power_w'] = 10**((6 - 30) / 10)
        point['output_phase_rad'] = math.radians(36.64)
        row = m.score(self.plan, samples)['axes']['sweep']['rows'][20]
        self.assertEqual(row['nominal_output_dbm'], 5)
        self.assertAlmostEqual(row['axis_output_dbm'], 6)
        self.assertAlmostEqual(row['hypotheses']['natural_cubic']['predicted_increment_deg'], 36.64)
        self.assertTrue(row['hypotheses']['nominal_zero_bypass']['passed'])

    def test_quantized_report_power_does_not_create_nominal_exception(self):
        samples = self.samples()
        dense = samples[-1]
        dense['ordinary']['output_power_w'] = .001
        dense['sweep'][11]['output_power_w'] = .001
        result = m.score(self.plan, samples)
        report = result['axes']['ordinary_report']['rows'][-1]
        raw = result['axes']['ordinary_raw']['rows'][-1]
        self.assertEqual(report['axis_output_dbm'], 0)
        self.assertEqual(report['nominal_output_dbm'], .001)
        for name in m.HYPOTHESES:
            self.assertFalse(report['hypotheses'][name]['bypass_at_registered_nominal_target'])
            self.assertFalse(report['hypotheses'][name]['passed'])
            self.assertGreater(report['hypotheses'][name]['circular_error_deg'], .005)
            self.assertTrue(raw['hypotheses'][name]['passed'])
        sweep_row = result['axes']['sweep']['rows'][6 * 61 + 11]
        self.assertFalse(sweep_row['hypotheses']['nominal_zero_bypass']['passed'])
        self.assertEqual(sweep_row['hypotheses']['nominal_zero_bypass']['predicted_increment_deg'], 10)

    def test_nominal_zero_bypass_preserves_measured_source_phase(self):
        samples = self.samples()
        samples[1]['ordinary']['raw_output_w'] *= (1 - 2.5e-8)
        result = m.score(self.plan, samples)
        row = result['axes']['ordinary_raw']['rows'][1]
        self.assertLess(row['axis_output_dbm'], 0)
        self.assertEqual(row['nominal_output_dbm'], 0)
        self.assertEqual(row['source_phase_deg'], 37)
        self.assertEqual(row['hypotheses']['nominal_zero_bypass']['predicted_phase_deg'], 37)
        self.assertTrue(row['hypotheses']['nominal_zero_bypass']['passed'])
        self.assertFalse(row['hypotheses']['natural_cubic']['passed'])
        samples[1]['ordinary']['output_phase_rad'] = 0
        row = m.score(self.plan, samples)['axes']['ordinary_report']['rows'][1]
        self.assertFalse(row['hypotheses']['nominal_zero_bypass']['passed'])

    def test_extra_final_points_are_retained_outside_planned_scores(self):
        result = m.score(self.plan, self.samples(extras=True))
        self.assertEqual(result['observed_sweep_point_count'], 387)
        self.assertEqual(len(result['additional_sweep_points']), 7)
        self.assertEqual(result['additional_sweep_points'][0]['index'], 61)
        self.assertEqual(result['additional_sweep_points'][0]['nominal_output_dbm'], 25.5)
        self.assertFalse(result['additional_points_in_planned_scores'])
        samples = self.samples(extras=True)
        samples[0]['additional_sweep_points'][0]['nominal_output_dbm'] += .5
        with self.assertRaises(ValueError):
            m.score(self.plan, samples)

    def test_empty_partial_reordered_and_missing_points_rejected(self):
        samples = self.samples()
        for changed in [[], samples[:1], samples[1:], list(reversed(samples))]:
            with self.subTest(count=len(changed)), self.assertRaises(ValueError):
                m.score(self.plan, changed)
        result = m.score(self.plan, samples[:1], allow_partial=True)
        self.assertFalse(result['all_planned_profiles_present'])
        self.assertEqual(result['observed_sweep_point_count'], 61)
        samples[0]['sweep'].pop()
        with self.assertRaises(ValueError):
            m.score(self.plan, samples)

    def test_observed_profile_grid_source_and_numeric_mutations_rejected(self):
        mutations = [
            lambda s: s[0]['profile'].update(source_phase_deg=False),
            lambda s: s[0]['sweep'][10].update(index=11),
            lambda s: s[0]['sweep'][10].update(nominal_output_dbm=.001),
            lambda s: s[0]['sweep'][10].update(input_power_w=.003),
            lambda s: s[1]['sweep'][10].update(input_phase_rad=math.radians(37)),
            lambda s: s[1]['ordinary'].update(input_phase_rad=0),
            lambda s: s[0]['ordinary'].update(raw_output_w=-1),
            lambda s: s[0]['sweep'][10].update(output_power_w=math.nan),
            lambda s: s[0]['sweep'][10].update(output_power_w=True),
            lambda s: s[0]['ordinary'].update(output_phase_rad=math.inf),
        ]
        for mutate in mutations:
            samples = self.samples()
            mutate(samples)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                m.score(self.plan, samples)

    def test_plan_mutations_tolerance_changes_and_missing_predictions_rejected(self):
        mutations = [
            lambda p: p.update(phase_absolute_tolerance_degrees=1e-4),
            lambda p: p.update(baseline_power_relative_tolerance=1e-6),
            lambda p: p['source_phase_conventions'].update(sweep='schematic_source_phase'),
            lambda p: p['profiles'][0].update(gain_db=21),
            lambda p: p['profiles'][2].update(ordinary_output_target_dbm=0),
            lambda p: p['profiles'][0].update(power_dbm=[0, 0, 20]),
            lambda p: p['profiles'][0].update(power_dbm=[0, math.inf, 20]),
            lambda p: p['profiles'][0].update(expected_points=True),
            lambda p: p['profiles'][0]['sweep_nominal_predictions'].pop(),
            lambda p: p['profiles'][0]['sweep_nominal_predictions'][10].update(nominal_zero_bypass=10),
            lambda p: p['hypotheses'].pop(),
            lambda p: p['profiles'].append(copy.deepcopy(p['profiles'][0])),
            lambda p: p['baseline_reference']['ordinary_observations'].update(sha256='0' * 64),
        ]
        for mutate in mutations:
            plan = copy.deepcopy(self.plan)
            mutate(plan)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                m.validate_plan(plan)

    def test_baseline_keeps_same_mode_and_cross_mode_controls_separate(self):
        baseline = m.compare_baseline(self.plan, self.baseline_samples()[0], self.prior, self.quick)
        self.assertTrue(baseline['all_comparisons_matched'])
        self.assertEqual(baseline['comparison_count'], 12)
        self.assertEqual({key: value['count'] for key, value in baseline['categories'].items()}, {
            'same_mode_ordinary_reproduction': 2,
            'cross_mode_sweep_to_ordinary': 7,
            'same_path_different_sweep_mode': 3})
        self.assertFalse(baseline['full_historical_sweep_reproduced'])
        self.assertFalse(baseline['vendor_compatibility_proven'])

    def test_baseline_failure_is_explicit_and_each_power_axis_is_kept(self):
        samples = self.baseline_samples()
        samples[0]['ordinary']['raw_output_w'] *= 1.01
        samples[0]['sweep'][20]['output_phase_rad'] += math.radians(.2)
        baseline = m.compare_baseline(self.plan, samples[0], self.prior, self.quick)
        self.assertFalse(baseline['all_comparisons_matched'])
        failed = [row for row in baseline['rows'] if not row['matched']]
        self.assertEqual([(r['category'], r['axis']) for r in failed], [
            ('same_mode_ordinary_reproduction', 'ordinary_raw'),
            ('cross_mode_sweep_to_ordinary', 'sweep_report')])

    def test_baseline_missing_or_duplicate_prior_points_rejected(self):
        sample = self.baseline_samples()[0]
        prior = copy.deepcopy(self.prior)
        prior['samples'] = [s for s in prior['samples']
                            if not (s['profile'] == 'interpolation-boundary' and s['target_output_dbm'] == 0)]
        with self.assertRaises(ValueError):
            m.compare_baseline(self.plan, sample, prior, self.quick)
        prior = copy.deepcopy(self.prior)
        prior['samples'].append(copy.deepcopy(prior['samples'][0]))
        with self.assertRaises(ValueError):
            m.compare_baseline(self.plan, sample, prior, self.quick)
        with self.assertRaises(ValueError):
            m.compare_baseline(self.plan, sample, self.prior, {'quick_sweeps': []})

    def test_cli_keeps_numerical_failure_report_and_refuses_overwrite_or_plan_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            evidence, output = temp / 'evidence.json', temp / 'result.json'
            samples = self.baseline_samples()
            samples[0]['ordinary']['raw_output_w'] *= 1.01
            evidence.write_text(json.dumps(dict(plan_sha256=m.PLAN_SHA256, samples=samples)), encoding='utf-8')
            command = [sys.executable, '-B', str(SCRIPT), str(PLAN_PATH), str(evidence), str(output), '--root', str(ROOT)]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(m.load(output)['baseline']['all_comparisons_matched'])
            retained = output.read_bytes()
            again = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(again.returncode, 0)
            self.assertEqual(output.read_bytes(), retained)
            changed_plan = temp / 'plan.json'
            changed_plan.write_bytes(PLAN_PATH.read_bytes() + b' ')
            command[3] = str(changed_plan)
            command[5] = str(temp / 'changed-result.json')
            changed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn('Frozen plan bytes changed', changed.stderr)
            self.assertFalse((temp / 'changed-result.json').exists())


if __name__ == '__main__':
    unittest.main()
