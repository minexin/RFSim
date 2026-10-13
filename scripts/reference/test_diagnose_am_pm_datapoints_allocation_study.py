"""Synthetic allocation checks; historical templates are not new vendor observations."""

import copy
from decimal import Decimal
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('diagnose-am-pm-datapoints-allocation-study.py')
ROOT = SCRIPT.resolve().parents[2]
PLAN = ROOT / 'validation/am-pm-datapoints-allocation-plan-20261014.json'
spec = importlib.util.spec_from_file_location('allocation_study_tests', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def set_output(point, *, raw_power=None, phase_degrees=None, dcp_power=None):
    if raw_power is not None:
        point['raw_output_w'] = raw_power
    if phase_degrees is not None:
        point['output_phase_rad'] = math.radians(phase_degrees)
    if dcp_power is not None:
        point['output_power_w'] = dcp_power
    phase, power = point['output_phase_rad'], point['raw_output_w']
    amplitude = math.sqrt(100 * power)
    for carrier in point['raw_carrier']:
        carrier.update(power_w=power, impedance=[50, 0],
                       voltage=[amplitude * math.cos(phase), amplitude * math.sin(phase)],
                       output_phase_deg=math.degrees(phase))
    point['raw_vs_report_relative_error'] = abs(power - point['output_power_w']) / max(power, point['output_power_w'])


def set_input_spectrum(point, raw_power=None):
    """Create a minimal, explicitly synthetic source-only two-carrier spectrum."""
    power = point['input_power_w'] if raw_power is None else raw_power
    phase = point['input_phase_rad']
    amplitude = math.sqrt(100 * power)
    voltage = [amplitude * math.cos(phase), amplitude * math.sin(phase)]
    label = '{1}D[synthetic-test],Source'
    vectors = dict(F1=[1e8 - .5, 1e8 + .5], P1=[power, power], V1=voltage * 2,
                   Z1=[50, 0, 50, 0], ID1=[17, 17], IDNo=[17], IDName=[label])
    point['raw_input_spectrum_nodes'] = [dict(path=m.normalizer.INPUT_PREFIX + name,
        data=copy.deepcopy(value), dimensions=[len(value)], synthetic_test_fixture=True)
        for name, value in vectors.items()]
    point['raw_input_carrier'] = [dict(raw_index=index, signal_id=17, frequency_hz=frequency,
        power_w=power, voltage=list(voltage), impedance=[50, 0], label=label)
        for index, frequency in enumerate(vectors['F1'])]
    point['raw_input_w'] = power / 2 + power / 2


def synthetic_samples(plan, history, *, behavior='output'):
    """Use archived G20 controls and independent analytic synthetic new configurations.

    New sweeps are arbitrary stable same-gain controls, not simulated vendor truth.
    Neither candidate scorer nor the implementation's spline generates phases.
    """
    historical = {sample['profile']['name']: sample for sample in history['samples']}
    control = historical['control-zero']
    samples = []
    for profile in plan['profiles']:
        q, gain = profile['ordinary_output_target_dbm'], profile['gain_db']
        sample = copy.deepcopy(control)
        sample.pop('capture_source', None)
        sample['profile'] = copy.deepcopy(profile)
        sample['parameters_si'] = m.previous.normalizer.expected_parameters(profile)
        sample['synthetic_test_fixture'] = True
        if gain == 20:
            name = {0: 'control-zero', -1e-6: 'negative-0p000001', 1e-6: 'positive-0p000001'}[q]
            sample['ordinary'] = copy.deepcopy(historical[name]['ordinary'])
        else:
            point = sample['ordinary']
            point['input_power_w'] = control['ordinary']['input_power_w'] * 10 ** ((q - gain + 20) / 10)
            power = .001 * 10 ** (q / 10)
            coordinate = 10 * math.log10(power) + 30
            cubic = 33.4375 + 3.5625 * coordinate - .3375 * coordinate ** 2 - .0225 * coordinate ** 3
            zero = (q in (0, -1e-6)) if behavior == 'output' else (profile['comparison_role'] == 'same_nominal_input')
            set_output(point, raw_power=power, dcp_power=power, phase_degrees=37 + (0 if zero else cubic))
            for point in sample['sweep'] + sample['additional_sweep_points']:
                point['nominal_output_dbm'] = float(Decimal(-30) + Decimal('.5') * point['index'] + Decimal(gain))
                point['output_power_w'] *= 10 ** ((gain - 20) / 10)
                point['output_phase_rad'] = math.radians(11 + point['nominal_output_dbm'] * .25)
        set_input_spectrum(sample['ordinary'])
        samples.append(sample)
    return samples


class AllocationStudyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = m.load(PLAN)
        cls.history = m.load(ROOT / m.REFERENCE['path'])

    def setUp(self):
        self.samples = synthetic_samples(self.plan, self.history)

    def score(self, samples=None, **kwargs):
        return m.score(self.plan, self.samples if samples is None else samples,
                       historical_observations=self.history, **kwargs)

    def test_frozen_registration_and_reference_identity(self):
        self.assertEqual(m.digest(PLAN), m.PLAN_SHA256)
        self.assertEqual(m.digest(ROOT / m.REFERENCE['path']), m.REFERENCE['sha256'])
        self.assertEqual(len(m.validate_plan(self.plan)), 12)
        self.assertEqual(self.plan['gain_control_indices'], [0, 3, 7])

    def test_contract_rejects_role_budget_coordinate_tolerance_control_and_type_changes(self):
        mutations = (
            lambda p: p['profiles'][3].update(gain_db=20),
            lambda p: p['profiles'][3].update(ordinary_role='historical_reproduction'),
            lambda p: p['profiles'][6].update(ordinary_output_target_dbm=-1),
            lambda p: p['profiles'][7].update(control_profile='g18-zero'),
            lambda p: p['profiles'][0].update(expected_points=61.0),
            lambda p: p['profiles'][0]['power_dbm'].__setitem__(0, False),
            lambda p: p['analysis_budget'].update(reserved_setup_recovery=False),
            lambda p: p.update(cross_gain_input_power_relative_tolerance=1e-10),
            lambda p: p.update(input_power_relative_tolerance='1e-10'),
            lambda p: p['repeated_sweep_indices'].append(3),
            lambda p: p['historical_baseline_indices'].__setitem__(1, 3),
            lambda p: p['primary_evaluation'].update(new_prediction_check_count=12),
            lambda p: p['transfer_zero_sets']['nominal_source_dbm'].__setitem__(1, '-20.0000009'),
            lambda p: p['hypothesis_definitions'].update(input_discrete_pattern='automatic fitted boundary'),
            lambda p: p['exclusions'].clear(),
            lambda p: p.update(unregistered=True),
            lambda p: p['profiles'].reverse(),
        )
        for index, mutate in enumerate(mutations):
            plan = copy.deepcopy(self.plan)
            mutate(plan)
            with self.subTest(index=index), self.assertRaises(ValueError):
                m.validate_plan(plan)

    def test_new_eight_controls_four_divergent_six_common_two_stay_separate(self):
        result = self.score()
        for key, count in (('new_prediction_checks', 8), ('historical_reproductions', 4),
                           ('divergent_prediction_checks', 6), ('common_nonzero_prediction_checks', 2)):
            group = result[key]
            self.assertEqual((group['planned_count'], group['observed_count'], group['missing_count']), (count, count, 0))
            self.assertTrue(all(item['point_count'] == count for item in group['summary'].values()))
        self.assertEqual(result['observed_sweep_point_count'], 732)
        self.assertEqual(result['additional_sweep_point_count'], 12)
        self.assertFalse(result['quick_sweeps_are_independent_prediction_checks'])
        self.assertFalse(result['additional_points_in_planned_scores'])

    def test_output_and_input_extensions_make_six_distinct_predictions(self):
        for behavior, passing, failing in (('output', 'output_discrete_pattern', 'input_discrete_pattern'),
                                           ('input', 'input_discrete_pattern', 'output_discrete_pattern')):
            result = self.score(synthetic_samples(self.plan, self.history, behavior=behavior))
            with self.subTest(behavior=behavior):
                summary = result['new_prediction_checks']['summary']
                self.assertEqual(summary[passing]['passed_count'], 8)
                self.assertEqual(summary[failing]['passed_count'], 2)
                self.assertTrue(result['complete_acquisition_controls_passed'])
                self.assertFalse(result['eligible_for_full_model_compatibility'])
                self.assertFalse(result['product_formula_changed'])

    def test_decimal_zero_sets_do_not_use_rounded_dcp_or_float_subtraction(self):
        for index in (1, 4, 9):
            row = m.score_point(self.plan['profiles'][index], self.samples[index]['ordinary'])
            with self.subTest(index=index):
                self.assertTrue(row['hypotheses']['output_discrete_pattern']['bypass_applied'])
                self.assertEqual(row['hypotheses']['input_discrete_pattern']['bypass_applied'], index == 1)
        profile = copy.deepcopy(self.plan['profiles'][1])
        profile['ordinary_output_target_dbm'] = -1e-6 + 1e-15
        row = m.score_point(profile, self.samples[1]['ordinary'])
        self.assertFalse(row['hypotheses']['output_discrete_pattern']['bypass_applied'])
        self.assertFalse(row['hypotheses']['input_discrete_pattern']['bypass_applied'])
        self.samples[5]['ordinary']['output_power_w'] = .001
        set_output(self.samples[5]['ordinary'])
        row = self.score()['new_prediction_checks']['rows'][2]
        self.assertFalse(row['hypotheses']['output_discrete_pattern']['bypass_applied'])
        self.assertFalse(row['hypotheses']['input_discrete_pattern']['bypass_applied'])

    def test_four_legacy_candidates_remain_separate_local_scores(self):
        result = self.score()
        self.assertEqual(result['legacy_hypotheses'], list(m.LEGACY_HYPOTHESES))
        self.assertEqual(set(result['new_prediction_checks']['legacy_summary']), set(m.LEGACY_HYPOTHESES))
        self.assertIn('failed', result['historical_hypothesis_status'])
        self.assertTrue(all(item['point_count'] == 8 for item in result['new_prediction_checks']['legacy_summary'].values()))
        self.assertEqual(set(result['new_prediction_checks']['summary']), set(m.HYPOTHESES))

    def test_finite_fingerprints_have_six_and_two_state_only_domains(self):
        # Changing a nonzero value preserves its fingerprint but fails both numeric predictions.
        point = self.samples[5]['ordinary']
        set_output(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
        result = self.score()
        output, source = result['finite_fingerprints']['output_pattern'], result['finite_fingerprints']['input_zero_pattern']
        self.assertEqual((output['planned_applicable_count'], output['observed_applicable_count']), (6, 6))
        self.assertEqual((source['planned_applicable_count'], source['observed_applicable_count']), (2, 2))
        self.assertTrue(output['complete_fingerprint_matched'])
        self.assertFalse(source['complete_fingerprint_matched'])
        self.assertEqual(result['new_prediction_checks']['summary']['output_discrete_pattern']['passed_count'], 7)

    def test_within_gain_uses_own_zero_and_cross_gain_allows_small_common_loading(self):
        for sample in self.samples[3:7]:
            sample['ordinary']['input_power_w'] *= 1 + 2e-8
        result = self.score()
        self.assertEqual([row['within_gain']['control_profile'] for row in result['input_identity_audit']],
                         ['g20-start-zero'] * 3 + ['g18-zero'] * 4 + ['g22-zero'] * 4 + ['g20-start-zero'])
        self.assertTrue(all(row['within_gain']['passed'] and row['cross_gain']['passed']
                            for row in result['input_identity_audit']))
        self.assertTrue(result['complete_acquisition_controls_passed'])

    def test_cross_gain_and_within_gain_failures_are_independent_prefix_gates(self):
        for sample in self.samples[3:7]:
            sample['ordinary']['input_power_w'] *= 1 + 2e-7
        with self.assertRaisesRegex(ValueError, 'across gains'):
            self.score(self.samples[:4], allow_partial=True)
        self.samples = synthetic_samples(self.plan, self.history)
        self.samples[4]['ordinary']['input_power_w'] *= 1 + 3e-10
        with self.assertRaisesRegex(ValueError, 'within gain g18'):
            self.score(self.samples[:5], allow_partial=True)
        self.samples = synthetic_samples(self.plan, self.history)
        self.samples[7]['ordinary']['input_power_w'] *= 1 + 2e-8
        with self.assertRaisesRegex(ValueError, 'within gain g22'):
            self.score()

    def test_raw_input_loading_and_gain_residuals_are_descriptive(self):
        point = self.samples[6]['ordinary']
        set_input_spectrum(point, point['raw_input_w'] * 1.02)
        result = self.score()
        diagnostic = result['raw_input_and_gain_diagnostics'][6]
        self.assertGreater(diagnostic['raw_vs_dcp_input_relative_error'], .01)
        self.assertGreater(diagnostic['raw_gain_relative_error'], .01)
        self.assertFalse(diagnostic['gates_acquisition'])
        self.assertTrue(result['complete_acquisition_controls_passed'])

    def test_power_equality_keeps_binary64_identity_and_nonzero_residuals(self):
        point = self.samples[6]['ordinary']
        original = self.samples[0]['ordinary']['raw_input_w']
        set_input_spectrum(point, math.nextafter(original, math.inf))
        result = self.score()
        pair = result['allocation_comparisons']['same_nominal_input_pairs']['rows'][0]
        self.assertTrue(pair['powers']['input_power_w']['exact_binary64_equal'])
        raw = pair['powers']['raw_input_w']
        self.assertFalse(raw['exact_binary64_equal'])
        self.assertNotEqual(raw['left_binary64_hex'], raw['right_binary64_hex'])
        self.assertGreater(raw['relative_error'], 0)
        self.assertLess(raw['relative_error'], 1e-10)
        self.assertFalse(result['allocation_comparisons']['same_nominal_input_pairs']['gates_acquisition'])
        self.assertFalse(result['allocation_comparisons']['same_nominal_output_pairs']['arbitrary_physical_power_function_refuted'])

    def test_same_gain_sweeps_use_three_controls_and_nine_repeats(self):
        result = self.score()
        rows = result['repeated_quick_sweeps']
        self.assertEqual([i for i, row in enumerate(rows) if row['initial_gain_reference']], [0, 3, 7])
        self.assertEqual([i for i, row in enumerate(rows) if row['new_auxiliary_reference']], [3, 7])
        self.assertEqual(result['completed_repeated_sweep_count'], 9)
        self.assertTrue(result['all_repeated_sweeps_matched'])
        self.assertNotEqual(self.samples[0]['sweep'], self.samples[3]['sweep'])
        with self.assertRaisesRegex(ValueError, 'same-gain control'):
            m.compare_repeated_sweep(self.plan, self.samples[4], self.samples[0])
        with self.assertRaisesRegex(ValueError, 'Initial gain reference'):
            m.compare_repeated_sweep(self.plan, self.samples[3], self.samples[3])

    def test_repeated_sweep_keeps_tight_input_and_separate_output_phase_gates(self):
        for field, factor, passed in (('input_power_w', 1 + 2e-10, False),
                                      ('output_power_w', 1 + 2e-8, True),
                                      ('output_power_w', 1 + 2e-7, False)):
            sample = copy.deepcopy(self.samples[4])
            sample['sweep'][20][field] *= factor
            with self.subTest(field=field, factor=factor):
                self.assertEqual(m.compare_repeated_sweep(self.plan, sample, self.samples[3])['all_comparisons_matched'], passed)
        self.samples[8]['additional_sweep_points'][0]['output_phase_rad'] += math.radians(.001)
        result = self.score()
        self.assertFalse(result['all_repeated_sweeps_matched'])
        self.assertFalse(result['complete_acquisition_controls_passed'])

    def test_only_two_g20_zero_controls_have_full_historical_baselines(self):
        result = self.score()
        self.assertEqual([row['profile'] for row in result['zero_baseline_controls']], ['g20-start-zero', 'g20-end-zero'])
        self.assertEqual([row['comparison_count'] for row in result['zero_baseline_controls']], [63, 63])
        for index in (1, 3, 7):
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'registered G20 zero'):
                m.compare_baseline(self.plan, self.samples[index], self.history)
        point = self.samples[-1]['ordinary']
        set_output(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
        result = self.score()
        self.assertFalse(result['all_observed_zero_baselines_matched'])
        self.assertFalse(result['complete_acquisition_controls_passed'])
        self.assertEqual(result['interpretation_status'], 'inconclusive')

    def test_scientific_failures_do_not_invalidate_acquisition(self):
        for index in list(range(3, 11)) + [1]:
            point = self.samples[index]['ordinary']
            set_output(point, phase_degrees=math.degrees(point['output_phase_rad']) + 1)
        result = self.score()
        self.assertEqual([row['passed_count'] for row in result['new_prediction_checks']['summary'].values()], [0, 0])
        self.assertFalse(result['historical_comparisons']['all_completed_comparisons_matched'])
        self.assertTrue(result['complete_acquisition_controls_passed'])
        self.assertEqual(result['observed_profile_count'], 12)
        self.assertFalse(result['eligible_for_vendor_compatibility'])

    def test_partial_prefixes_count_missing_controls_pairs_and_predictions(self):
        for count in (1, 3, 4, 6, 7, 8, 10, 11):
            result = self.score(self.samples[:count], allow_partial=True)
            with self.subTest(count=count):
                self.assertEqual(result['sequence_status'], 'incomplete')
                self.assertEqual(result['interpretation_status'], 'inconclusive')
                self.assertFalse(result['complete_acquisition_controls_passed'])
                self.assertEqual(result['missing_profile_count'], 12 - count)
                self.assertEqual(result['new_prediction_checks']['observed_count'], max(0, count - 3))
                self.assertEqual(result['completed_zero_baseline_count'], 1)
                self.assertEqual(result['missing_zero_baseline_profiles'], ['g20-end-zero'])
                self.assertEqual(result['completed_repeated_sweep_count'], sum(i < count for i in self.plan['repeated_sweep_indices']))
                for key, pairs in self.plan['allocation_comparisons'].items():
                    if not key.endswith('_pairs'):
                        continue
                    present = {p['name'] for p in self.plan['profiles'][:count]}
                    completed = sum(left in present and right in present for left, right in pairs)
                    self.assertEqual(result['allocation_comparisons'][key]['completed_pair_count'], completed)
                    self.assertEqual(result['allocation_comparisons'][key]['missing_pair_count'], len(pairs) - completed)

    def test_missing_historical_reference_never_implies_complete_controls(self):
        result = m.score(self.plan, self.samples)
        self.assertFalse(result['historical_comparisons']['reference_provided'])
        self.assertEqual(result['historical_comparisons']['missing_count'], 4)
        self.assertEqual(result['completed_zero_baseline_count'], 0)
        self.assertFalse(result['complete_acquisition_controls_passed'])
        self.assertEqual(result['interpretation_status'], 'inconclusive')
        changed = copy.deepcopy(self.history)
        changed['plan_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'Historical observation plan identity'):
            m.score(self.plan, self.samples, historical_observations=changed)

    def test_invalid_acquisition_order_raw_input_and_coverage_are_rejected(self):
        for mutation in ('order', 'role', 'raw input voltage', 'raw input mean', 'raw output', 'count', 'empty'):
            samples = copy.deepcopy(self.samples)
            if mutation == 'order':
                samples[3], samples[4] = samples[4], samples[3]
            elif mutation == 'role':
                samples[3]['profile']['ordinary_role'] = 'historical_reproduction'
            elif mutation == 'raw input voltage':
                samples[3]['ordinary']['raw_input_spectrum_nodes'][2]['data'][0] *= 2
            elif mutation == 'raw input mean':
                samples[3]['ordinary']['raw_input_w'] *= 2
            elif mutation == 'raw output':
                samples[3]['ordinary']['raw_carrier'][0]['voltage'][0] *= 2
            elif mutation == 'count':
                samples.pop()
            else:
                samples = []
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                m.score(self.plan, samples)

    def test_scoring_preserves_registration_samples_and_historical_bytes(self):
        before = copy.deepcopy((self.plan, self.samples, self.history))
        self.score()
        self.assertEqual((self.plan, self.samples, self.history), before)
        self.assertEqual(m.digest(ROOT / m.REFERENCE['path']), m.REFERENCE['sha256'])

    def test_cli_pins_inputs_preserves_scientific_failure_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            plan_path, history_path, evidence, output = [folder / name for name in
                ('plan.json', 'history.json', 'evidence.json', 'report.json')]
            plan_path.write_bytes(PLAN.read_bytes())
            history_path.write_bytes((ROOT / m.REFERENCE['path']).read_bytes())
            set_output(self.samples[3]['ordinary'], phase_degrees=38)
            evidence.write_text(json.dumps(dict(plan_sha256=m.PLAN_SHA256, samples=self.samples)), encoding='utf-8')
            command = [sys.executable, '-I', '-S', '-B', str(SCRIPT), str(plan_path), str(evidence), str(output),
                       '--reference', str(history_path)]
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            original = output.read_bytes()
            result = json.loads(original)
            self.assertEqual(result['new_prediction_checks']['summary']['output_discrete_pattern']['passed_count'], 7)
            self.assertFalse(result['raw_sources_verified'])
            self.assertTrue(result['baseline_file_hashes_verified'])
            self.assertNotEqual(subprocess.run(command, capture_output=True, check=False).returncode, 0)
            self.assertEqual(output.read_bytes(), original)
            for source in (plan_path, history_path):
                original_input = source.read_bytes()
                source.write_bytes(original_input + b'\n')
                destination = folder / ('rejected-' + source.stem + '.json')
                rejected_command = command.copy()
                rejected_command[7] = str(destination)
                rejected = subprocess.run(rejected_command, capture_output=True, text=True, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertFalse(destination.exists())
                source.write_bytes(original_input)

    def test_cli_partial_mode_is_explicit_and_remains_inconclusive(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            evidence, output = folder / 'prefix.json', folder / 'report.json'
            evidence.write_text(json.dumps(dict(plan_sha256=m.PLAN_SHA256, samples=self.samples[:7])), encoding='utf-8')
            command = [sys.executable, '-I', '-S', '-B', str(SCRIPT), str(PLAN), str(evidence), str(output), '--root', str(ROOT)]
            self.assertNotEqual(subprocess.run(command, capture_output=True, check=False).returncode, 0)
            self.assertFalse(output.exists())
            completed = subprocess.run(command + ['--allow-partial'], capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(output.read_bytes())
            self.assertEqual(result['interpretation_status'], 'inconclusive')
            self.assertEqual(result['new_prediction_checks']['observed_count'], 4)
            self.assertFalse(result['complete_acquisition_controls_passed'])


if __name__ == '__main__':
    unittest.main()
