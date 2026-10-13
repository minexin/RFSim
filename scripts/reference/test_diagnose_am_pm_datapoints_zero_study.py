"""Independent synthetic discrimination, fixed-plan guards, and baseline controls."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import shutil
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
OBSERVATIONS_PATH = ROOT / 'validation/am-pm-datapoints-zero-observations-20261013.json'
CAPTURES_PATH = ROOT / 'validation/am-pm-datapoints-zero-captures-20261013.json'
DIAGNOSTIC_PATH = ROOT / 'validation/am-pm-datapoints-zero-diagnostic-20261013.json'
CAPTURES_SHA256 = 'f0b35c70acd99536d74855b07d9713e80b70c1b4edda32aba20a32818fca62a3'
OBSERVATIONS_SHA256 = '77a8712efd6e000cc70fe93ce6d586adae803c35674cb334f4de8cb0d518fef7'
DIAGNOSTIC_SHA256 = '5662e7f38ce167cdb04dee8f7ee5baf03826b17291a7bee8049a18d2e58122fc'


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

    def test_cli_raw_root_requires_capture_manifest_before_any_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'result.json'
            result = subprocess.run(
                [sys.executable, '-B', str(SCRIPT), str(PLAN_PATH),
                 str(Path(directory) / 'unused-evidence.json'), str(output), '--raw-root', str(ROOT)],
                capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Raw verification requires a capture manifest', result.stderr)
            self.assertFalse(output.exists())


class DataPointsZeroCaptureRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = m.load(PLAN_PATH)
        cls.observations = m.load(OBSERVATIONS_PATH)
        cls.samples = cls.observations['samples']
        cls.by_name = {sample['profile']['name']: sample for sample in cls.samples}
        cls.result = m.score(cls.plan, cls.samples)
        cls.baseline = m.compare_baseline(
            cls.plan, cls.samples[0], m.load(ROOT / m.REFERENCES['ordinary_observations']['path']),
            m.load(ROOT / m.REFERENCES['quick_sweep_evidence']['path']))
        cls.diagnostic = m.load(DIAGNOSTIC_PATH)

    def test_archived_evidence_byte_identities_and_links_are_frozen(self):
        for path, wanted in [(CAPTURES_PATH, CAPTURES_SHA256),
                             (OBSERVATIONS_PATH, OBSERVATIONS_SHA256),
                             (DIAGNOSTIC_PATH, DIAGNOSTIC_SHA256)]:
            self.assertEqual(m.digest(path), wanted, str(path))
        self.assertEqual(self.observations['plan_sha256'], m.PLAN_SHA256)
        self.assertEqual(self.observations['capture_manifest_sha256'], CAPTURES_SHA256)
        self.assertEqual(self.diagnostic['capture_manifest_sha256'], CAPTURES_SHA256)
        self.assertEqual(self.diagnostic['evidence_sha256'], OBSERVATIONS_SHA256)

    def assert_replayed(self, actual, expected, path='report'):
        """Allow only libm rounding; fields, types, order and verdicts stay exact."""
        self.assertIs(type(actual), type(expected), path)
        if isinstance(actual, dict):
            self.assertEqual(list(actual), list(expected), path)
            for key in actual:
                self.assert_replayed(actual[key], expected[key], path + '/' + key)
        elif isinstance(actual, list):
            self.assertEqual(len(actual), len(expected), path)
            for index, (a, b) in enumerate(zip(actual, expected)):
                self.assert_replayed(a, b, path + '/' + str(index))
        elif isinstance(actual, float):
            self.assertTrue(math.isfinite(actual) and math.isfinite(expected), path)
            field = path.rsplit('/', 1)[-1]
            if 'tolerance' in field or field == 'nominal_output_dbm':
                self.assertEqual(actual, expected, path)
            else:
                delta = 1e-15 if field.endswith('_w') or 'relative_error' in field else 1e-11
                self.assertAlmostEqual(actual, expected, delta=delta, msg=path)
        else:
            self.assertEqual(actual, expected, path)

    def test_real_capture_counts_and_all_three_failed_complete_candidates(self):
        self.assertEqual(len(self.samples), 7)
        self.assertEqual([len(sample['sweep']) for sample in self.samples], [61] * 6 + [21])
        self.assertEqual([sample['profile']['name'] for sample in self.samples],
                         [profile['name'] for profile in self.plan['profiles']])
        self.assertTrue(self.result['all_planned_profiles_present'])
        self.assertEqual(self.result['observed_sweep_point_count'], 387)
        expected = {'sweep': (387, [376, 383, 375]),
                    'ordinary_report': (7, [1, 6, 2]), 'ordinary_raw': (7, [2, 7, 3])}
        for axis, (count, passed) in expected.items():
            summary = self.result['axes'][axis]['summary']
            self.assertEqual([summary[name]['passed_count'] for name in m.HYPOTHESES], passed)
            self.assertTrue(all(summary[name]['point_count'] == count for name in m.HYPOTHESES))
        self.assertFalse(any(row['passed'] for row in self.result['axes']['sweep']['summary'].values()))
        self.assertFalse(self.result['eligible_for_vendor_compatibility'])
        self.assertFalse(self.result['product_formula_changed'])
        self.assertEqual(self.result['parameters_fitted_this_study'], 0)

    def test_four_report_failures_and_nine_equal_report_powers_with_different_phases(self):
        failed = [row for row in self.result['axes']['sweep']['rows']
                  if not row['hypotheses']['nominal_zero_bypass']['passed']]
        self.assertEqual([(row['profile'], row['index'], row['nominal_output_dbm']) for row in failed],
                         [('dense-zero-neighborhood', 11 + i, (i + 1) / 1000) for i in range(4)])
        self.assertTrue(all(row['axis_output_dbm'] == 0 for row in failed))
        dense = self.by_name['dense-zero-neighborhood']
        plateau = [row for row in dense['sweep'] if row['output_power_w'] == .001]
        self.assertEqual([row['nominal_output_dbm'] for row in plateau],
                         [-.004, -.003, -.002, -.001, 0, .001, .002, .003, .004])
        for point in plateau[:4]:
            self.assertAlmostEqual(math.degrees(point['output_phase_rad']), 10, delta=1e-5)
        self.assertEqual(plateau[4]['output_phase_rad'], 0)
        positive_phases = [math.degrees(point['output_phase_rad']) for point in plateau[5:]]
        self.assertTrue(all(10 < a < b for a, b in zip(positive_phases, positive_phases[1:])))
        ordinary_report = self.result['axes']['ordinary_report']['rows'][-1]
        ordinary_raw = self.result['axes']['ordinary_raw']['rows'][-1]
        self.assertFalse(ordinary_report['hypotheses']['nominal_zero_bypass']['passed'])
        self.assertTrue(ordinary_raw['hypotheses']['nominal_zero_bypass']['passed'])
        self.assertGreater(ordinary_report['hypotheses']['nominal_zero_bypass']['circular_error_deg'], .005)
        self.assertLess(ordinary_raw['hypotheses']['nominal_zero_bypass']['circular_error_deg'], 1e-8)

    def test_shifted_first_knots_are_counterexamples_to_first_knot_bypass(self):
        for name, power, phase in [
                ('first-knot-negative', -5, 10), ('first-knot-positive', 5, 10),
                ('zero-interior-knot', -10, 15), ('constant-phase-loss', -5, 20)]:
            sample = self.by_name[name]
            point = next(row for row in sample['sweep'] if row['nominal_output_dbm'] == power)
            self.assertAlmostEqual(math.degrees(point['output_phase_rad']), phase, delta=1e-5)
            zero = next(row for row in sample['sweep'] if row['nominal_output_dbm'] == 0)
            self.assertEqual(zero['output_phase_rad'], 0)
        positive = self.by_name['first-knot-positive']
        self.assertEqual(positive['sweep'][15]['nominal_output_dbm'], 5)
        self.assertEqual(positive['sweep'][5]['nominal_output_dbm'], 0)
        self.assertAlmostEqual(math.degrees(positive['ordinary']['output_phase_rad']), -35, delta=1e-5)
        row = self.result['axes']['ordinary_report']['rows'][2]
        self.assertFalse(row['hypotheses']['nominal_first_knot_bypass']['passed'])
        self.assertGreater(row['hypotheses']['nominal_first_knot_bypass']['circular_error_deg'], 9.99)

    def test_nominal_zero_preserves_source_and_is_not_exact_raw_power_equality(self):
        for name, phase in [('original-triangle', 0), ('first-knot-negative', 37),
                            ('zero-interior-knot', -29), ('constant-phase-loss', 123),
                            ('unity-gain-control', 63)]:
            ordinary = self.by_name[name]['ordinary']
            self.assertAlmostEqual(math.degrees(ordinary['input_phase_rad']), phase, delta=1e-12)
            self.assertEqual(ordinary['output_phase_rad'], ordinary['input_phase_rad'])
            self.assertEqual(ordinary['output_power_w'], .001)
            self.assertNotEqual(ordinary['raw_output_w'], .001)
            if name == 'unity-gain-control':
                self.assertGreater(ordinary['raw_output_w'], .001)
            else:
                self.assertLess(ordinary['raw_output_w'], .001)
        self.assertTrue(all(point['input_phase_rad'] == 0
                            for sample in self.samples for point in sample['sweep']))

    def test_unity_and_dense_power_disagreements_remain_separate_from_phase_passes(self):
        errors = {sample['profile']['name']: m.relative(sample['ordinary']['raw_output_w'],
                                                       sample['ordinary']['output_power_w'])
                  for sample in self.samples}
        self.assertEqual({name for name, value in errors.items() if value > 1e-7},
                         {'unity-gain-control', 'dense-zero-neighborhood'})
        self.assertAlmostEqual(errors['unity-gain-control'], 2.0498267584768674e-7, delta=1e-15)
        self.assertAlmostEqual(errors['dense-zero-neighborhood'], .00023020699272687645, delta=1e-15)
        for sample in self.samples:
            self.assertAlmostEqual(sample['ordinary']['raw_vs_report_relative_error'],
                                   errors[sample['profile']['name']], delta=1e-15)
        unity_row = self.result['axes']['ordinary_raw']['rows'][5]
        self.assertTrue(unity_row['hypotheses']['nominal_zero_bypass']['passed'])

    def test_real_baseline_identity_classes_and_extra_endpoints_are_retained(self):
        self.assertEqual(self.baseline['comparison_count'], 12)
        self.assertTrue(self.baseline['all_comparisons_matched'])
        self.assertEqual({key: value['count'] for key, value in self.baseline['categories'].items()},
                         {'same_mode_ordinary_reproduction': 2, 'cross_mode_sweep_to_ordinary': 7,
                          'same_path_different_sweep_mode': 3})
        self.assertFalse(self.baseline['full_historical_sweep_reproduced'])
        extras = self.result['additional_sweep_points']
        self.assertEqual(len(extras), 7)
        self.assertEqual([len(sample['additional_sweep_points']) for sample in self.samples], [1] * 7)
        self.assertEqual([row['index'] for row in extras], [61] * 6 + [21])
        self.assertEqual([row['nominal_output_dbm'] for row in extras],
                         [25.5, 20.5, 28.0, 15.5, 20.5, 25.5, .011])
        self.assertFalse(self.result['additional_points_in_planned_scores'])
        self.assertEqual(self.result['axes']['sweep']['summary']['nominal_zero_bypass']['point_count'], 387)

    def test_committed_diagnostic_recomputes_without_claiming_current_raw_verification(self):
        recomputed = copy.deepcopy(self.result)
        self.assertFalse(recomputed['raw_sources_verified'])
        recomputed['baseline'] = self.baseline
        recomputed.update(plan_sha256=m.digest(PLAN_PATH), evidence_sha256=m.digest(OBSERVATIONS_PATH),
                          baseline_file_hashes_verified=True)
        # The archived diagnostic was generated with local raw verification. This
        # normalized replay has no such evidence and must not inherit that claim.
        expected = copy.deepcopy(self.diagnostic)
        self.assertTrue(expected.pop('acquisition_archive_verified'))
        self.assertEqual(expected.pop('capture_manifest_sha256'), m.digest(CAPTURES_PATH))
        self.assertTrue(expected['raw_sources_verified'])
        expected['raw_sources_verified'] = False
        self.assert_replayed(recomputed, expected)

    def test_cli_committed_chain_succeeds_without_any_retained_raw_capture_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            validation = root / 'validation'
            validation.mkdir()
            # Only the committed evidence chain is available in this isolated
            # root. There is deliberately no build-reference directory.
            for source in [PLAN_PATH, OBSERVATIONS_PATH, CAPTURES_PATH,
                           ROOT / 'validation/am-pm-datapoints-zero-restoration-20261013.json',
                           ROOT / 'validation/am-pm-datapoints-zero-workflow-repairs-20261013.json',
                           *(ROOT / reference['path'] for reference in m.REFERENCES.values())]:
                shutil.copyfile(source, validation / source.name)
            output = root / 'offline-result.json'
            command = [sys.executable, '-I', '-S', '-B', str(SCRIPT),
                       str(validation / PLAN_PATH.name), str(validation / OBSERVATIONS_PATH.name),
                       str(output), '--root', str(root),
                       '--capture-manifest', str(validation / CAPTURES_PATH.name)]
            process = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(process.returncode, 0, process.stderr)
            replay = m.load(output)
            self.assertFalse(replay['raw_sources_verified'])
            self.assertTrue(replay['acquisition_archive_verified'])
            self.assertTrue(replay['baseline']['all_comparisons_matched'])
            self.assertFalse((root / 'build-reference').exists())
            historical = copy.deepcopy(self.diagnostic)
            self.assertTrue(historical['raw_sources_verified'])
            historical['raw_sources_verified'] = False
            self.assert_replayed(replay, historical)


if __name__ == '__main__':
    unittest.main()
