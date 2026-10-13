"""Mandatory real allocation-report replay; no SystemVue, COM or raw-workspace access."""

import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name('report-am-pm-datapoints-allocation-study.py')
ROOT = SCRIPT.resolve().parents[2]
spec = importlib.util.spec_from_file_location('allocation_report_tests', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
PLAN = ROOT / 'validation/am-pm-datapoints-allocation-plan-20261014.json'
OBSERVATIONS = ROOT / 'validation/am-pm-datapoints-allocation-observations-20261014.json'
MANIFEST = ROOT / 'validation/am-pm-datapoints-allocation-captures-20261014.json'
DIAGNOSTIC = ROOT / 'validation/am-pm-datapoints-allocation-diagnostic-20261014.json'
WORKFLOW = ROOT / 'validation/am-pm-datapoints-allocation-workflow-20261014.json'
REFERENCE = ROOT / m.scorer.REFERENCE['path']
FIXTURES = (PLAN, OBSERVATIONS, MANIFEST, WORKFLOW, REFERENCE)


def isolated_inputs(directory):
    validation = directory / 'validation'
    validation.mkdir()
    for source in FIXTURES:
        shutil.copyfile(source, validation / source.name)
    return validation


def command(directory, validation, output):
    return [sys.executable, '-I', '-S', '-B', str(SCRIPT), str(validation / PLAN.name),
            str(validation / OBSERVATIONS.name), str(output), '--capture-manifest',
            str(validation / MANIFEST.name), '--root', str(directory)]


class AllocationReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = m.report(ROOT, PLAN, OBSERVATIONS, MANIFEST)
        cls.plan = m.scorer.load(PLAN)

    def test_eight_new_four_historical_and_six_two_subsets_have_correct_denominators(self):
        for key, count in (('historical_reproductions', 4), ('new_prediction_checks', 8),
                           ('divergent_prediction_checks', 6), ('common_nonzero_prediction_checks', 2)):
            group = self.result[key]
            self.assertEqual((group['planned_count'], group['observed_count'], group['missing_count']),
                             (count, count, 0))
            self.assertTrue(group['complete'])
            self.assertEqual(group['missing_profile_names'], [])
            self.assertTrue(all(item['point_count'] == count for item in group['summary'].values()))
        new = self.result['new_prediction_checks']
        self.assertEqual([row['profile'] for row in new['rows']],
                         self.plan['primary_evaluation']['new_prediction_profile_names'])
        for name, passed in zip(m.scorer.HYPOTHESES, (8, 2)):
            summary = new['summary'][name]
            self.assertEqual((summary['point_count'], summary['passed_count'], summary['passed']),
                             (8, passed, passed == 8))
        self.assertLess(new['summary']['output_discrete_pattern']['max_circular_error_deg'], 1e-6)
        for name, passed in zip(m.scorer.LEGACY_HYPOTHESES, (4, 4, 6, 6)):
            self.assertEqual((new['legacy_summary'][name]['point_count'], new['legacy_summary'][name]['passed_count']),
                             (8, passed))
            self.assertFalse(new['legacy_summary'][name]['passed'])
        self.assertIn('failed', self.result['historical_hypothesis_status'])
        self.assertFalse(self.result['eligible_for_vendor_compatibility'])
        self.assertFalse(self.result['eligible_for_full_model_compatibility'])
        self.assertFalse(self.result['product_formula_changed'])

    def test_zero_profiles_follow_acquisition_order_and_crosses_remain_nonzero(self):
        self.assertEqual(self.result['observed_zero_increment_profiles'],
            ['g20-start-zero', 'g20-negative-epsilon', 'g18-zero', 'g18-negative-epsilon',
             'g22-zero', 'g22-negative-epsilon', 'g20-end-zero'])
        expected_dcp_zero = [profile['name'] for profile in self.plan['profiles']
                             if profile['comparison_role'] != 'same_nominal_input']
        self.assertEqual(self.result['observed_exact_dcp_one_milliwatt_profiles'], expected_dcp_zero)
        rows = {row['profile']: row for group in ('historical_reproductions', 'new_prediction_checks')
                for row in self.result[group]['rows']}
        for group in ('g18', 'g22'):
            self.assertTrue(rows[group + '-zero']['zero_increment'])
            self.assertTrue(rows[group + '-negative-epsilon']['zero_increment'])
            self.assertFalse(rows[group + '-positive-epsilon']['zero_increment'])
            cross = rows[group + '-fixed-input']
            self.assertFalse(cross['zero_increment'])
            self.assertEqual(cross['nominal_source_dbm'], '-20')
            self.assertTrue(cross['hypotheses']['output_discrete_pattern']['passed'])
            self.assertFalse(cross['hypotheses']['input_discrete_pattern']['passed'])
        self.assertAlmostEqual(rows['g18-fixed-input']['observed_increment_deg'], 25.142499491666253, places=10)
        self.assertAlmostEqual(rows['g22-fixed-input']['observed_increment_deg'], 39.032499783594304, places=10)
        fingerprints = self.result['finite_fingerprints']
        for name, count, matched in (('output_pattern', 6, True), ('input_zero_pattern', 2, False)):
            item = fingerprints[name]
            self.assertEqual((item['planned_applicable_count'], item['observed_applicable_count'],
                              item['missing_applicable_count']), (count, count, 0))
            self.assertIs(item['complete_fingerprint_matched'], matched)
            self.assertTrue(item['evaluates_only_zero_nonzero_state'])
            self.assertFalse(item['gates_acquisition'])

    def test_same_nominal_pairs_preserve_exact_raw_power_differences(self):
        output = self.result['allocation_comparisons']['same_nominal_output_pairs']
        source = self.result['allocation_comparisons']['same_nominal_input_pairs']
        for group, count in ((output, 6), (source, 2)):
            self.assertEqual((group['planned_pair_count'], group['completed_pair_count'], group['missing_pair_count']),
                             (count, count, 0))
            self.assertFalse(group['gates_acquisition'])
            self.assertFalse(group['arbitrary_physical_power_function_refuted'])
            for pair in group['rows']:
                self.assertEqual(set(pair['powers']), {'input_power_w', 'output_power_w', 'raw_input_w', 'raw_output_w'})
                for power in pair['powers'].values():
                    self.assertEqual(power['left_binary64_hex'], power['left_w'].hex())
                    self.assertEqual(power['right_binary64_hex'], power['right_w'].hex())
                    self.assertIs(power['exact_binary64_equal'],
                                  power['left_binary64_hex'] == power['right_binary64_hex'])
        for pair in output['rows']:
            self.assertTrue(pair['powers']['output_power_w']['exact_binary64_equal'])
            self.assertFalse(pair['powers']['input_power_w']['exact_binary64_equal'])
            self.assertFalse(pair['powers']['raw_input_w']['exact_binary64_equal'])
            raw = pair['powers']['raw_output_w']
            self.assertFalse(raw['exact_binary64_equal'])
            self.assertGreater(raw['relative_error'], 0)
            self.assertLess(raw['relative_error'], 1e-10)
            self.assertTrue(pair['phases']['increment']['matched'])
        self.assertEqual([pair['powers']['input_power_w']['exact_binary64_equal'] for pair in source['rows']],
                         [False, True])
        for pair in source['rows']:
            raw = pair['powers']['raw_input_w']
            self.assertFalse(raw['exact_binary64_equal'])
            self.assertGreater(raw['relative_error'], 0)
            self.assertLess(raw['relative_error'], 1e-14)
            self.assertFalse(pair['powers']['raw_output_w']['exact_binary64_equal'])
            self.assertFalse(pair['phases']['increment']['matched'])
            self.assertFalse(pair['zero_increment_states_equal'])

    def test_gain_specific_controls_complete_coverage_and_offline_scope(self):
        result = self.result
        audit = result['input_identity_audit']
        expected_controls = ['g20-start-zero'] * 3 + ['g18-zero'] * 4 + ['g22-zero'] * 4 + ['g20-start-zero']
        self.assertEqual([row['within_gain']['control_profile'] for row in audit], expected_controls)
        self.assertEqual([row['cross_gain']['control_profile'] for row in audit], ['g20-start-zero'] * 12)
        for row in audit:
            self.assertTrue(row['within_gain']['passed'])
            self.assertTrue(row['cross_gain']['passed'])
            self.assertEqual(row['within_gain']['relative_tolerance'], 1e-10)
            self.assertEqual(row['cross_gain']['relative_tolerance'], 1e-7)
        repeats = result['repeated_quick_sweeps']
        self.assertEqual([index for index, row in enumerate(repeats) if row['initial_gain_reference']], [0, 3, 7])
        self.assertEqual([index for index, row in enumerate(repeats) if row['new_auxiliary_reference']], [3, 7])
        self.assertEqual([row['profile'] for row in result['zero_baseline_controls']], ['g20-start-zero', 'g20-end-zero'])
        self.assertEqual([row['comparison_count'] for row in result['zero_baseline_controls']], [63, 63])
        self.assertTrue(all(row['all_comparisons_matched'] for row in result['zero_baseline_controls']))
        self.assertEqual(result['completed_repeated_sweep_count'], 9)
        self.assertEqual(result['missing_repeated_sweep_profiles'], [])
        self.assertEqual(result['missing_zero_baseline_profiles'], [])
        self.assertTrue(result['all_repeated_sweeps_matched'])
        self.assertTrue(result['complete_acquisition_controls_passed'])
        self.assertEqual(result['sequence_status'], 'complete')
        self.assertEqual(result['interpretation_status'], 'complete_finite_candidate_results')
        self.assertEqual(result['missing_profile_count'], 0)
        self.assertEqual(result['historical_comparisons']['completed_count'], 4)
        self.assertTrue(result['historical_comparisons']['complete_historical_reproduction_matched'])
        self.assertEqual(result['same_configuration_comparisons']['completed_pair_count'], 1)
        self.assertEqual(result['observed_sweep_point_count'], 732)
        self.assertEqual(result['additional_sweep_point_count'], 12)
        self.assertFalse(result['quick_sweeps_are_independent_prediction_checks'])
        self.assertTrue(result['acquisition_archive_verified'])
        self.assertFalse(result['raw_sources_verified'])
        for row in result['raw_input_and_gain_diagnostics']:
            self.assertFalse(row['gates_acquisition'])
            self.assertGreater(row['raw_vs_dcp_input_relative_error'], 0)
        verification = result['acquisition_verification']
        self.assertFalse(verification['raw_source_bytes_verified'])
        self.assertEqual(verification['invocation_count'], 15)
        self.assertEqual(verification['parameter_readbacks_per_capture'], 18)
        self.assertEqual(verification['raw_input_nodes_per_capture'], 7)
        self.assertEqual(verification['raw_input_carriers_per_capture'], 2)
        self.assertEqual(verification['restored_parameter_occurrences'], 247)
        self.assertEqual(result['plan_sha256'], m.scorer.PLAN_SHA256)
        self.assertEqual(result['historical_reference_sha256'], m.scorer.REFERENCE['sha256'])

    def test_retained_raw_verified_diagnostic_matches_portable_offline_replay(self):
        retained = m.scorer.load(DIAGNOSTIC)
        self.assertTrue(retained['raw_sources_verified'])
        self.assertTrue(retained['acquisition_verification']['raw_source_bytes_verified'])
        retained['raw_sources_verified'] = False
        retained['acquisition_verification']['raw_source_bytes_verified'] = False
        m.archive.compare_replay(retained, self.result)

    def test_isolated_cli_needs_no_raw_files_and_refuses_overwrite_or_fake_raw_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            validation = isolated_inputs(root)
            before = {source.name: (validation / source.name).read_bytes() for source in FIXTURES}
            output = root / 'report.json'
            cmd = command(root, validation, output)
            completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            original = output.read_bytes()
            self.assertFalse(json.loads(original)['raw_sources_verified'])
            self.assertFalse((root / 'build-reference').exists())
            self.assertNotEqual(subprocess.run(cmd, capture_output=True, check=False).returncode, 0)
            self.assertEqual(output.read_bytes(), original)
            missing_raw = root / 'missing-raw-report.json'
            rejected = subprocess.run(command(root, validation, missing_raw) + ['--raw-root', str(root)],
                                      capture_output=True, text=True, check=False)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse(missing_raw.exists())
            self.assertEqual({source.name: (validation / source.name).read_bytes() for source in FIXTURES}, before)

    def test_cli_exit_depends_on_acquisition_gates_not_scientific_matches(self):
        # Modified reports below are control-flow fixtures, not altered vendor evidence.
        with tempfile.TemporaryDirectory() as temporary:
            for gate in (None, 'all_observed_zero_baselines_matched', 'all_repeated_sweeps_matched',
                         'all_planned_profiles_present', 'complete_acquisition_controls_passed',
                         'completed_repeated_sweep_count', 'zero_baseline_controls'):
                result = copy.deepcopy(self.result)
                for item in result['new_prediction_checks']['summary'].values():
                    item.update(passed=False, passed_count=0)
                result['historical_comparisons']['all_completed_comparisons_matched'] = False
                result['historical_comparisons']['complete_historical_reproduction_matched'] = False
                for item in result['finite_fingerprints'].values():
                    item.update(complete_fingerprint_matched=False, all_observed_states_matched=False)
                if gate == 'completed_repeated_sweep_count':
                    result[gate] = 8
                elif gate == 'zero_baseline_controls':
                    result[gate].pop()
                elif gate is not None:
                    result[gate] = False
                output = Path(temporary) / (str(gate) + '.json')
                args = [str(SCRIPT), str(PLAN), str(OBSERVATIONS), str(output),
                        '--capture-manifest', str(MANIFEST), '--root', str(ROOT)]
                with self.subTest(gate=gate), mock.patch.object(m, 'report', return_value=result), \
                        mock.patch.object(sys, 'argv', args), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(m.main(), 0 if gate is None else 1)
                saved = json.loads(output.read_bytes())
                self.assertTrue(all(not item['passed'] for item in saved['new_prediction_checks']['summary'].values()))
                self.assertFalse(saved['finite_fingerprints']['output_pattern']['complete_fingerprint_matched'])

    def test_changed_plan_history_workflow_or_source_links_block_output(self):
        for kind in ('plan', 'reference', 'workflow', 'observation-link', 'raw-source-link'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                validation = isolated_inputs(root)
                if kind in ('plan', 'reference', 'workflow'):
                    source = {'plan': PLAN, 'reference': REFERENCE, 'workflow': WORKFLOW}[kind]
                    target = validation / source.name
                    target.write_bytes(target.read_bytes() + b'\n')
                else:
                    target = validation / OBSERVATIONS.name
                    observations = json.loads(target.read_bytes())
                    if kind == 'observation-link':
                        observations['capture_manifest_sha256'] = '0' * 64
                    else:
                        observations['samples'][-1]['capture_source']['capture_sha256'] = '0' * 64
                    target.write_text(json.dumps(observations), encoding='utf-8')
                output = root / 'rejected.json'
                rejected = subprocess.run(command(root, validation, output), capture_output=True, text=True, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertFalse(output.exists())

    def test_missing_mandatory_committed_evidence_never_falls_back_to_partial_report(self):
        for source in FIXTURES:
            with self.subTest(missing=source.name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                validation = isolated_inputs(root)
                (validation / source.name).unlink()
                output = root / 'incomplete.json'
                rejected = subprocess.run(command(root, validation, output), capture_output=True, text=True, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
