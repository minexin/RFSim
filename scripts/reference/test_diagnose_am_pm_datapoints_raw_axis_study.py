"""Offline tests for the preregistered ordinary raw-power-axis discrimination."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name('diagnose-am-pm-datapoints-raw-axis-study.py')
PLAN_PATH = ROOT / 'validation/am-pm-datapoints-raw-axis-plan-20261013.json'
spec = importlib.util.spec_from_file_location('raw_axis_study', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class RawAxisStudyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = m.load(PLAN_PATH)
        cls.prior = m.load(ROOT / m.REFERENCE['path'])
        cls.old = next(s for s in cls.prior['samples'] if s['profile']['name'] == 'first-knot-negative')

    @staticmethod
    def set_raw(point, power, phase):
        point['raw_output_w'] = power
        point['output_phase_rad'] = math.radians(phase)
        for carrier in point['raw_carrier']:
            carrier['power_w'] = power
            carrier['voltage'] = [math.sqrt(100 * power) * math.cos(math.radians(phase)),
                                  math.sqrt(100 * power) * math.sin(math.radians(phase))]
            carrier['impedance'] = [50., 0.]
            carrier['output_phase_deg'] = math.degrees(math.atan2(carrier['voltage'][1], carrier['voltage'][0]))
        point['raw_vs_report_relative_error'] = m.relative(power, point['output_power_w'])

    def samples(self):
        samples = []
        for index, profile in enumerate(self.plan['profiles']):
            sample = copy.deepcopy(self.old)
            sample['profile'] = copy.deepcopy(profile)
            sample['parameters_si'] = m.normalizer.expected_parameters(profile)
            target = profile['ordinary_output_target_dbm']
            ordinary = sample['ordinary']
            ordinary['input_power_w'] = self.old['ordinary']['input_power_w'] * 10 ** (target / 10)
            if index:
                raw = self.old['ordinary']['raw_output_w'] * 10 ** (target / 10)
                ordinary['output_power_w'] = .001 if abs(target) <= .004 else raw
                axis = 10 * math.log10(raw) + 30
                phase = 37 + m.hyp.natural_cubic(profile['power_dbm'], profile['phase_deg'], axis)
                self.set_raw(ordinary, raw, phase)
            samples.append(sample)
        return samples

    def test_frozen_plan_and_historical_reference_bytes(self):
        self.assertEqual(m.digest(PLAN_PATH), m.PLAN_SHA256)
        self.assertEqual(m.digest(ROOT / m.REFERENCE['path']), m.REFERENCE['sha256'])
        self.assertEqual(len(m.validate_plan(self.plan)), 11)

    def test_primary_ten_checks_exclude_control_and_all_repeated_sweeps(self):
        result = m.score(self.plan, self.samples())
        self.assertEqual(result['observed_new_prediction_check_count'], 10)
        self.assertEqual(result['observed_sweep_point_count'], 671)
        self.assertEqual(len(result['repeated_quick_sweeps']), 11)
        self.assertEqual(len(result['reproduction_controls']['rows']), 1)
        self.assertTrue(all(s['point_count'] == 10 for s in result['new_prediction_checks']['summary'].values()))
        self.assertTrue(all(s['point_count'] == 1 for s in result['reproduction_controls']['summary'].values()))
        self.assertTrue(result['all_repeated_sweeps_matched'])
        self.assertFalse(result['quick_sweeps_are_independent_prediction_checks'])
        self.assertFalse(result['additional_points_in_planned_scores'])
        self.assertFalse(result['raw_sources_verified'])
        self.assertFalse(result['eligible_for_vendor_compatibility'])
        self.assertFalse(result['product_formula_changed'])
        self.assertEqual(result['parameters_fitted_this_study'], 0)

    def test_synthetic_raw_continuous_and_nominal_bypass_are_distinct_from_report_gate(self):
        result = m.score(self.plan, self.samples())
        summary = result['new_prediction_checks']['summary']
        self.assertEqual(summary['raw_continuous']['passed_count'], 10)
        self.assertEqual(summary['raw_nominal_zero_bypass']['passed_count'], 10)
        self.assertEqual(summary['raw_report_zero_bypass']['passed_count'], 4)
        self.assertLess(summary['dcp_continuous']['passed_count'], 10)
        control = result['reproduction_controls']['rows'][0]
        self.assertTrue(control['zero_increment'])
        self.assertTrue(control['hypotheses']['raw_nominal_zero_bypass']['passed'])
        self.assertFalse(control['hypotheses']['raw_continuous']['passed'])
        self.assertNotEqual(control['raw_output_power_w'], .001)
        self.assertAlmostEqual(control['source_phase_deg'], 37, delta=1e-12)

    def test_report_gate_uses_exact_equality_not_rounding_or_raw_equality(self):
        profile = self.plan['profiles'][4]
        point = self.samples()[4]['ordinary']
        point['output_power_w'] = math.nextafter(.001, 0)
        row = m.score_point(profile, point)
        self.assertFalse(row['hypotheses']['raw_report_zero_bypass']['bypass_applied'])
        point['output_power_w'] = .001
        row = m.score_point(profile, point)
        self.assertTrue(row['hypotheses']['raw_report_zero_bypass']['bypass_applied'])
        self.assertFalse(row['hypotheses']['raw_nominal_zero_bypass']['bypass_applied'])

    def test_tiny_targets_classify_nonzero_increment_without_claiming_axis_resolution(self):
        rows = m.score(self.plan, self.samples())['new_prediction_checks']['rows']
        tiny = [row for row in rows if abs(row['nominal_output_dbm']) == .000001]
        self.assertEqual(len(tiny), 2)
        self.assertTrue(all(not row['zero_increment'] for row in tiny))
        self.assertTrue(all(row['hypotheses']['dcp_continuous']['passed'] for row in tiny))
        self.assertTrue(all(row['hypotheses']['raw_continuous']['passed'] for row in tiny))

    def test_natural_cubic_matches_independent_analytic_polynomial(self):
        for x in [-5, -.005, -.0045, -.000001, 0, .000001, .0045, 5]:
            polynomial = 33.4375 + 3.5625*x - .3375*x*x - .0225*x*x*x
            self.assertAlmostEqual(m.hyp.natural_cubic([-5, 5, 15], [10, 40, -20], x), polynomial, delta=1e-12)

    def test_ratio_input_identity_accepts_historical_bias_and_rejects_wrong_tiny_target(self):
        samples = self.samples()
        bias = m.relative(samples[0]['ordinary']['input_power_w'], 1e-5)
        self.assertGreater(bias, m.INPUT_TOLERANCE)
        self.assertLess(bias, m.POWER_TOLERANCE)
        m.validate_samples(self.plan, samples)
        samples[5]['ordinary']['input_power_w'] = samples[0]['ordinary']['input_power_w']
        with self.assertRaisesRegex(ValueError, 'input-power ratio'):
            m.score(self.plan, samples)

    def test_ratio_input_identity_threshold_is_independent_of_dcp_output(self):
        samples = self.samples()
        samples[1]['ordinary']['input_power_w'] *= 1 + 2e-10
        with self.assertRaisesRegex(ValueError, 'input-power ratio'):
            m.validate_samples(self.plan, samples)
        samples = self.samples()
        samples[1]['ordinary']['input_power_w'] *= 1 + .5e-10
        m.validate_samples(self.plan, samples)

    def test_same_mode_complete_baseline_includes_all_power_axes_and_extra_endpoint(self):
        baseline = m.compare_baseline(self.plan, self.samples()[0], self.prior)
        self.assertTrue(baseline['all_comparisons_matched'])
        self.assertTrue(baseline['full_historical_sweep_reproduced'])
        self.assertEqual(baseline['comparison_count'], 63)
        self.assertEqual(set(baseline['rows'][0]['powers']), {'input_power_w', 'output_power_w', 'raw_output_w'})
        self.assertEqual([r['index'] for r in baseline['rows'] if r['category'] == 'planned_sweep'], list(range(61)))
        self.assertEqual(baseline['rows'][-1]['category'], 'additional_sweep')
        self.assertFalse(baseline['vendor_compatibility_proven'])

    def test_numerical_baseline_failure_is_retained(self):
        samples = self.samples()
        point = samples[0]['ordinary']
        self.set_raw(point, point['raw_output_w'] * (1 + 2e-7), 37)
        samples[0]['sweep'][9]['output_phase_rad'] += math.radians(.001)
        result = m.compare_baseline(self.plan, samples[0], self.prior)
        self.assertFalse(result['all_comparisons_matched'])
        self.assertEqual([(r['category'], r['index']) for r in result['rows'] if not r['matched']],
                         [('ordinary', 0), ('planned_sweep', 9)])

    def test_repeated_sweep_uses_its_own_control_input_ratio_and_retains_numerical_failure(self):
        samples = self.samples()
        # Historical ordinary and sweep input quantization differ; same-index
        # control comparison must not substitute the ordinary control Pin.
        self.assertGreater(m.relative(samples[0]['sweep'][60]['input_power_w'],
                                      samples[0]['ordinary']['input_power_w'] * 100), m.INPUT_TOLERANCE)
        self.assertTrue(m.compare_repeated_sweep(self.plan, samples[1], samples[0])['all_comparisons_matched'])
        samples[1]['sweep'][20]['input_power_w'] *= 1 + 2e-10
        result = m.score(self.plan, samples)
        self.assertFalse(result['all_repeated_sweeps_matched'])
        row = result['repeated_quick_sweeps'][1]['comparison']['rows'][20]
        self.assertFalse(row['powers']['input_power_w']['matched'])
        self.assertTrue(row['powers']['output_power_w']['matched'])

    def test_repeated_sweep_phase_and_output_power_failures_are_separate(self):
        samples = self.samples()
        samples[2]['sweep'][3]['output_power_w'] *= 1 + 2e-7
        samples[2]['sweep'][4]['output_phase_rad'] += math.radians(.001)
        report = m.compare_repeated_sweep(self.plan, samples[2], samples[0])
        self.assertFalse(report['all_comparisons_matched'])
        self.assertEqual([r['index'] for r in report['rows'] if not r['matched']], [3, 4])

    def test_partial_is_explicit_ordered_and_has_no_primary_pass_with_control_alone(self):
        samples = self.samples()
        result = m.score(self.plan, samples[:1], allow_partial=True)
        self.assertFalse(result['all_planned_profiles_present'])
        self.assertTrue(all(row['point_count'] == 0 and not row['passed'] for row in result['new_prediction_checks']['summary'].values()))
        for bad, partial in [(samples[:1], False), ([], True), (samples[1:2], True), (samples + samples[:1], False)]:
            with self.subTest(length=len(bad), partial=partial), self.assertRaises(ValueError):
                m.score(self.plan, bad, allow_partial=partial)

    def test_registered_plan_mutations_are_rejected(self):
        mutations = [lambda p: p.update(input_power_relative_tolerance=1e-7),
                     lambda p: p.update(phase_absolute_tolerance_degrees=1e-4),
                     lambda p: p['profiles'][5].update(ordinary_output_target_dbm=0),
                     lambda p: p['profiles'][1].update(ordinary_role='reproduction_control'),
                     lambda p: p['profiles'][0].update(gain_db=True),
                     lambda p: p['profiles'][0].update(expected_points=61.),
                     lambda p: p['analysis_budget'].update(total_maximum=13),
                     lambda p: p['quick_sweeps'].update(required_parameter_readback='Enable optional'),
                     lambda p: p['primary_evaluation'].update(zero_control_excluded_from_new_prediction_pass_rates=False),
                     lambda p: p['local_spline_polynomial'].update(ascending_coefficients=[33.4376, 3.5625, -.3375, -.0225]),
                     lambda p: p['baseline_reference'].update(sha256='0'*64)]
        for mutate in mutations:
            plan = copy.deepcopy(self.plan)
            mutate(plan)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                m.validate_plan(plan)

    def test_observation_structure_parameters_and_raw_physics_fail_closed(self):
        mutations = [lambda s: s[1]['ordinary'].update(input_phase_rad=0),
                     lambda s: s[1]['ordinary'].update(output_power_w=math.nan),
                     lambda s: s[1]['ordinary'].update(raw_output_w=True),
                     lambda s: s[1]['ordinary']['raw_carrier'][0].update(power_w=.5),
                     lambda s: s[1]['ordinary']['raw_carrier'][0].update(frequency_hz=1e8),
                     lambda s: s[1]['ordinary']['raw_carrier'][0].update(output_phase_deg=0),
                     lambda s: s[1]['ordinary'].update(raw_vs_report_relative_error=.5),
                     lambda s: s[1]['parameters_si'].update({m.normalizer.SOURCE+'Pwr': 1e-5}),
                     lambda s: s[1]['sweep'][5].update(index=6),
                     lambda s: s[1]['sweep'][5].update(nominal_output_dbm=0),
                     lambda s: s[1]['sweep'].pop(),
                     lambda s: s[1]['additional_sweep_points'].append(copy.deepcopy(s[1]['additional_sweep_points'][0]))]
        for mutate in mutations:
            samples = self.samples()
            mutate(samples)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                m.score(self.plan, samples)

    def test_cli_replays_without_raw_claim_and_refuses_overwrite_or_changed_plan(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            evidence, output = temp/'evidence.json', temp/'report.json'
            evidence.write_text(json.dumps(dict(plan_sha256=m.PLAN_SHA256, samples=self.samples())), encoding='utf-8')
            command = [sys.executable, '-I', '-S', '-B', str(SCRIPT), str(PLAN_PATH), str(evidence), str(output)]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(m.load(output)['raw_sources_verified'])
            retained = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), retained)
            changed_plan = temp/'plan.json'
            changed_plan.write_bytes(PLAN_PATH.read_bytes() + b' ')
            command[5], command[7] = str(changed_plan), str(temp/'changed-report.json')
            changed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn('Frozen plan bytes changed', changed.stderr)
            self.assertFalse((temp/'changed-report.json').exists())


if __name__ == '__main__':
    unittest.main()
