"""Synthetic order-study checks; historical values are templates, never new measurements."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('diagnose-am-pm-datapoints-order-study.py')
ROOT = SCRIPT.resolve().parents[2]
PLAN = ROOT / 'validation/am-pm-datapoints-order-plan-20261014.json'
spec = importlib.util.spec_from_file_location('order_study', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def set_ordinary(point, *, input_power=None, output_power=None, raw_power=None, phase_degrees=None):
    """Construct a coherent synthetic carrier when a test changes an ordinary point."""
    if input_power is not None:
        point['input_power_w'] = input_power
    if output_power is not None:
        point['output_power_w'] = output_power
    if raw_power is not None:
        point['raw_output_w'] = raw_power
    if phase_degrees is not None:
        point['output_phase_rad'] = math.radians(phase_degrees)
    phase = point['output_phase_rad']
    power = point['raw_output_w']
    amplitude = math.sqrt(100 * power)
    for carrier in point['raw_carrier']:
        carrier.update(power_w=power, impedance=[50, 0],
                       voltage=[amplitude * math.cos(phase), amplitude * math.sin(phase)],
                       output_phase_deg=math.degrees(phase))
    point['raw_vs_report_relative_error'] = abs(power - point['output_power_w']) / max(power, point['output_power_w'])


def synthetic_samples(plan, history):
    """Reuse old controls; new +/-delta values use the independent analytic cubic."""
    by_name = {sample['profile']['name']: sample for sample in history['samples']}
    control = by_name['control-zero']
    result = []
    for profile in plan['profiles']:
        sample = copy.deepcopy(control)
        target = profile['ordinary_output_target_dbm']
        if target in (0, -1e-6, 1e-6):
            name = {0: 'control-zero', -1e-6: 'negative-0p000001', 1e-6: 'positive-0p000001'}[target]
            sample['ordinary'] = copy.deepcopy(by_name[name]['ordinary'])
        else:
            # Independent polynomial; do not call the implementation's spline.
            increment = 33.4375 + 3.5625 * target - .3375 * target ** 2 - .0225 * target ** 3
            ratio = 10 ** (target / 10)
            set_ordinary(sample['ordinary'], input_power=control['ordinary']['input_power_w'] * ratio,
                         output_power=.001 * ratio, raw_power=.001 * ratio,
                         phase_degrees=37 + increment)
        sample['profile'] = copy.deepcopy(profile)
        sample['parameters_si'] = m.previous.normalizer.expected_parameters(profile)
        sample.pop('capture_source', None)
        sample['synthetic_test_fixture'] = True
        result.append(sample)
    return result


class OrderStudyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = m.load(PLAN)
        cls.history = m.load(ROOT / m.REFERENCE['path'])

    def setUp(self):
        self.samples = synthetic_samples(self.plan, self.history)

    def score(self, samples=None, **options):
        return m.score(self.plan, self.samples if samples is None else samples,
                       historical_observations=self.history, **options)

    def test_exact_registration_and_historical_source_hashes(self):
        self.assertEqual(m.digest(PLAN), m.PLAN_SHA256)
        self.assertEqual(m.digest(ROOT / m.REFERENCE['path']), m.REFERENCE['sha256'])
        self.assertEqual(len(m.validate_plan(self.plan)), 10)
        self.assertEqual([profile['block_position'] for profile in self.plan['profiles']], list(range(5)) * 2)

    def test_registration_rejects_wrong_roles_blocks_order_and_numeric_types(self):
        mutations = (
            ('profile-role', lambda p: p['profiles'][7].update(ordinary_role='new_prediction_check')),
            ('profile-block', lambda p: p['profiles'][5].update(block='a')),
            ('profile-position', lambda p: p['profiles'][5].update(block_position=False)),
            ('profile-position-float', lambda p: p['profiles'][5].update(block_position=0.0)),
            ('profile-count-float', lambda p: p['profiles'][0].update(expected_points=61.0)),
            ('profile-order', lambda p: p['profiles'].reverse()),
            ('profile-target', lambda p: p['profiles'][2].update(ordinary_output_target_dbm=2e-5)),
            ('profile-extra', lambda p: p['profiles'][0].update(unregistered=True)),
            ('table-bool', lambda p: p['profiles'][0]['power_dbm'].__setitem__(0, False)),
            ('tolerance-string', lambda p: p.update(input_power_relative_tolerance='1e-10')),
            ('tolerance-change', lambda p: p.update(ordinary_replication_power_relative_tolerance=1e-7)),
            ('budget-bool', lambda p: p['analysis_budget'].update(reserved_setup_recovery=False)),
            ('new-count', lambda p: p['primary_evaluation'].update(new_prediction_check_count=4)),
            ('source-bool', lambda p: p['source_phase_conventions'].update(nominal_sweep_source_phase_degrees=False)),
            ('pair-order', lambda p: p['order_comparisons']['pairs'].reverse()),
            ('comparison-role', lambda p: p['order_comparisons'].update(mismatches_are_results_not_acquisition_failures=False)),
            ('hypothesis-rule', lambda p: p['hypothesis_definitions'].update(raw_nominal_zero_bypass='different rule')),
        )
        for name, mutate in mutations:
            plan = copy.deepcopy(self.plan)
            mutate(plan)
            with self.subTest(mutation=name), self.assertRaises(ValueError):
                m.validate_plan(plan)

    def test_six_two_two_roles_and_only_two_new_predictions(self):
        result = self.score()
        for key, count in (('historical_reproductions', 6), ('new_prediction_checks', 2), ('order_reproductions', 2)):
            self.assertEqual(result[key]['planned_count'], count)
            self.assertEqual(result[key]['observed_count'], count)
            self.assertEqual(result[key]['missing_count'], 0)
            self.assertTrue(all(summary['point_count'] == count for summary in result[key]['summary'].values()))
        self.assertEqual([row['profile'] for row in result['new_prediction_checks']['rows']],
                         ['a-positive-delta', 'a-negative-delta'])
        self.assertEqual(result['new_prediction_checks']['summary']['raw_continuous']['passed_count'], 2)
        self.assertEqual(result['order_comparisons']['completed_pair_count'], 5)
        self.assertEqual(result['historical_comparisons']['completed_count'], 6)
        self.assertEqual(len(result['zero_baseline_controls']), 2)
        self.assertTrue(result['all_planned_profiles_present'])
        self.assertTrue(result['complete_sequence_replication_matched'])
        self.assertFalse(result['eligible_for_full_model_compatibility'])
        self.assertFalse(result['quick_sweeps_are_independent_prediction_checks'])

    def test_analytic_delta_fixture_is_independent_of_spline_and_source_offset(self):
        result = self.score()
        for row in result['new_prediction_checks']['rows']:
            target = row['nominal_output_dbm']
            expected = 33.4375 + 3.5625 * target - .3375 * target ** 2 - .0225 * target ** 3
            self.assertAlmostEqual(row['observed_increment_deg'], expected, places=11)
            self.assertAlmostEqual(row['source_phase_deg'], 37, places=12)
            self.assertTrue(row['hypotheses']['raw_continuous']['passed'])

    def test_block_b_input_identity_uses_its_own_zero_control(self):
        for sample in self.samples[5:]:
            sample['ordinary']['input_power_w'] *= 1 + 2e-8
        result = self.score()
        self.assertEqual([row['control_profile'] for row in result['input_identity_audit']],
                         ['a-zero'] * 5 + ['b-zero'] * 5)
        self.assertTrue(all(row['passed'] for row in result['input_identity_audit']))
        self.assertTrue(result['all_observed_zero_baselines_matched'])
        self.assertFalse(result['order_comparisons']['all_completed_pairs_matched'])
        self.assertFalse(result['complete_sequence_replication_matched'])

    def test_wrong_block_input_denominator_cannot_hide_ratio_failure(self):
        self.samples[5]['ordinary']['input_power_w'] *= 1 + 2e-8
        with self.assertRaisesRegex(ValueError, 'ratio mismatch within block b'):
            self.score()
        self.samples = synthetic_samples(self.plan, self.history)
        self.samples[8]['ordinary']['input_power_w'] *= 1 + 3e-10
        with self.assertRaisesRegex(ValueError, 'ratio mismatch within block b'):
            self.score()

    def test_both_zero_controls_accept_historical_baseline_and_second_zero_can_fail(self):
        for index in (0, 5):
            comparison = m.compare_baseline(self.plan, self.samples[index], self.history)
            self.assertTrue(comparison['all_comparisons_matched'])
            self.assertEqual(comparison['profile'], ('a-zero' if index == 0 else 'b-zero'))
        with self.assertRaisesRegex(ValueError, 'requires a registered block zero'):
            m.compare_baseline(self.plan, self.samples[1], self.history)
        point = self.samples[5]['ordinary']
        set_ordinary(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
        comparison = m.compare_baseline(self.plan, self.samples[5], self.history)
        self.assertFalse(comparison['all_comparisons_matched'])
        self.assertFalse(self.score()['all_observed_zero_baselines_matched'])

    def test_all_sweeps_including_b_zero_compare_to_a_zero(self):
        result = self.score()
        repeats = result['repeated_quick_sweeps'][1:]
        self.assertEqual(len(repeats), 9)
        self.assertTrue(all(row['comparison']['reference_profile'] == 'a-zero' for row in repeats))
        self.samples[5]['sweep'][20]['output_phase_rad'] += math.radians(.001)
        result = self.score()
        self.assertFalse(result['all_repeated_sweeps_matched'])
        self.assertFalse(result['all_observed_zero_baselines_matched'])
        with self.assertRaises(ValueError):
            m.compare_repeated_sweep(self.plan, self.samples[6], self.samples[5])

    def test_repeated_sweep_uses_tight_input_and_separate_output_tolerances(self):
        self.samples[1]['sweep'][20]['input_power_w'] *= 1 + 2e-10
        self.assertFalse(m.compare_repeated_sweep(self.plan, self.samples[1], self.samples[0])['all_comparisons_matched'])
        self.samples = synthetic_samples(self.plan, self.history)
        self.samples[1]['sweep'][20]['output_power_w'] *= 1 + 2e-8
        self.assertTrue(m.compare_repeated_sweep(self.plan, self.samples[1], self.samples[0])['all_comparisons_matched'])
        self.samples[1]['sweep'][20]['output_power_w'] *= 1 + 2e-7
        self.assertFalse(m.compare_repeated_sweep(self.plan, self.samples[1], self.samples[0])['all_comparisons_matched'])

    def test_scientific_prediction_historical_and_order_differences_are_reported(self):
        # A new prediction failure is valid synthetic evidence, not an exception.
        point = self.samples[2]['ordinary']
        set_ordinary(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
        # Replicating a historical epsilon can disagree while all sweeps remain sound.
        point = self.samples[4]['ordinary']
        set_ordinary(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
        # A paired ordinary power difference uses 1e-10, not the looser sweep gate.
        point = self.samples[7]['ordinary']
        set_ordinary(point, output_power=point['output_power_w'] * (1 + 2e-8),
                     raw_power=point['raw_output_w'] * (1 + 2e-8))
        result = self.score()
        self.assertTrue(result['all_repeated_sweeps_matched'])
        self.assertTrue(result['all_observed_zero_baselines_matched'])
        self.assertEqual(result['new_prediction_checks']['summary']['raw_continuous']['passed_count'], 1)
        self.assertFalse(result['historical_comparisons']['all_completed_comparisons_matched'])
        self.assertFalse(result['order_comparisons']['all_completed_pairs_matched'])
        self.assertEqual(result['observed_profile_count'], 10)

    def test_partial_prefix_reports_missing_profiles_and_completed_pairs(self):
        for count, pairs in ((1, 0), (3, 0), (6, 1), (8, 3), (9, 4)):
            result = self.score(self.samples[:count], allow_partial=True)
            with self.subTest(count=count):
                self.assertEqual(result['sequence_status'], 'incomplete')
                self.assertEqual(result['missing_profile_count'], 10 - count)
                self.assertEqual(result['order_comparisons']['completed_pair_count'], pairs)
                self.assertEqual(result['order_comparisons']['missing_pair_count'], 5 - pairs)
                self.assertFalse(result['all_planned_profiles_present'])
                self.assertFalse(result['complete_sequence_replication_matched'])
                self.assertFalse(result['order_comparisons']['complete_order_reproduction_matched'])
                self.assertTrue(result['all_repeated_sweeps_matched'])

    def test_historical_context_is_explicit_and_never_assumed_when_missing(self):
        result = m.score(self.plan, self.samples)
        self.assertFalse(result['historical_comparisons']['reference_provided'])
        self.assertEqual(result['historical_comparisons']['completed_count'], 0)
        self.assertEqual(result['historical_comparisons']['missing_count'], 6)
        self.assertEqual(result['zero_baseline_controls'], [])
        self.assertFalse(result['complete_sequence_replication_matched'])
        history = copy.deepcopy(self.history)
        history['plan_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'Historical observation plan identity changed'):
            m.score(self.plan, self.samples, historical_observations=history)

    def test_invalid_acquisition_order_roles_raw_phasors_and_coverage_are_rejected(self):
        for mutation in ('order', 'role', 'raw', 'count', 'empty'):
            samples = copy.deepcopy(self.samples)
            if mutation == 'order':
                samples[1], samples[2] = samples[2], samples[1]
            elif mutation == 'role':
                samples[7]['profile']['ordinary_role'] = 'new_prediction_check'
            elif mutation == 'raw':
                samples[2]['ordinary']['raw_carrier'][0]['voltage'][0] *= 2
            elif mutation == 'count':
                samples.pop()
            else:
                samples = []
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                m.score(self.plan, samples)

    def test_scoring_does_not_mutate_registration_or_observations(self):
        before = copy.deepcopy((self.plan, self.samples, self.history))
        self.score()
        self.assertEqual((self.plan, self.samples, self.history), before)

    def test_cli_pins_plan_and_reference_never_overwrites_and_keeps_scientific_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            plan_path, history_path, evidence, output = [folder / name for name in
                ('plan.json', 'history.json', 'evidence.json', 'report.json')]
            plan_path.write_bytes(PLAN.read_bytes())
            history_path.write_bytes((ROOT / m.REFERENCE['path']).read_bytes())
            point = self.samples[2]['ordinary']
            set_ordinary(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
            evidence.write_text(json.dumps(dict(plan_sha256=m.PLAN_SHA256, samples=self.samples)), encoding='utf-8')
            command = [sys.executable, '-I', '-S', str(SCRIPT), str(plan_path), str(evidence), str(output),
                       '--reference', str(history_path)]
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            saved = output.read_bytes()
            result = json.loads(saved)
            self.assertEqual(result['new_prediction_checks']['summary']['raw_continuous']['passed_count'], 1)
            self.assertFalse(result['raw_sources_verified'])
            self.assertTrue(result['baseline_file_hashes_verified'])
            repeated = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(repeated.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)
            for source in (plan_path, history_path):
                original = source.read_bytes()
                source.write_bytes(original + b'\n')
                destination = folder / ('rejected-' + source.stem + '.json')
                changed = command.copy(); changed[6] = str(destination)
                rejected = subprocess.run(changed, capture_output=True, text=True, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertFalse(destination.exists())
                source.write_bytes(original)

    def test_cli_partial_mode_is_explicit_and_reports_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            evidence, output = folder / 'prefix.json', folder / 'prefix-report.json'
            evidence.write_text(json.dumps(dict(plan_sha256=m.PLAN_SHA256, samples=self.samples[:6])), encoding='utf-8')
            command = [sys.executable, '-I', '-S', str(SCRIPT), str(PLAN), str(evidence), str(output), '--root', str(ROOT)]
            rejected = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse(output.exists())
            accepted = subprocess.run(command + ['--allow-partial'], capture_output=True, text=True, check=False)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            result = json.loads(output.read_bytes())
            self.assertEqual(result['sequence_status'], 'incomplete')
            self.assertEqual(result['order_comparisons']['completed_pair_count'], 1)
            self.assertFalse(result['complete_sequence_replication_matched'])


if __name__ == '__main__':
    unittest.main()
