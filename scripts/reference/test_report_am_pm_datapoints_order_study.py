"""Mandatory real-data order-report replay; no SystemVue or COM invocation."""

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


SCRIPT = Path(__file__).with_name('report-am-pm-datapoints-order-study.py')
ROOT = SCRIPT.resolve().parents[2]
spec = importlib.util.spec_from_file_location('order_report_tests', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
PLAN = ROOT / 'validation/am-pm-datapoints-order-plan-20261014.json'
OBSERVATIONS = ROOT / 'validation/am-pm-datapoints-order-observations-20261014.json'
MANIFEST = ROOT / 'validation/am-pm-datapoints-order-captures-20261014.json'
DIAGNOSTIC = ROOT / 'validation/am-pm-datapoints-order-diagnostic-20261014.json'
WORKFLOW = ROOT / 'validation/am-pm-datapoints-order-workflow-20261014.json'
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


class OrderReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = m.report(ROOT, PLAN, OBSERVATIONS, MANIFEST)

    def test_two_new_predictions_are_separate_from_six_historical_and_two_order_repeats(self):
        for key, count in (('historical_reproductions', 6), ('new_prediction_checks', 2), ('order_reproductions', 2)):
            self.assertEqual(self.result[key]['planned_count'], count)
            self.assertEqual(self.result[key]['observed_count'], count)
            self.assertEqual(self.result[key]['missing_count'], 0)
        for hypothesis, passed in zip(m.scorer.HYPOTHESES, (0, 2, 2, 0)):
            summary = self.result['new_prediction_checks']['summary'][hypothesis]
            self.assertEqual((summary['point_count'], summary['passed_count'], summary['passed']),
                             (2, passed, passed == 2))
        self.assertEqual([row['profile'] for row in self.result['new_prediction_checks']['rows']],
                         ['a-positive-delta', 'a-negative-delta'])
        self.assertFalse(self.result['eligible_for_vendor_compatibility'])
        self.assertFalse(self.result['eligible_for_full_model_compatibility'])
        self.assertFalse(self.result['product_formula_changed'])

    def test_zero_increment_profiles_keep_acquisition_order_and_raw_axis_evidence(self):
        self.assertEqual(self.result['observed_zero_increment_profiles'],
                         ['a-zero', 'a-negative-epsilon', 'b-zero', 'b-negative-epsilon'])
        rows = {row['profile']: row for group in ('historical_reproductions', 'new_prediction_checks', 'order_reproductions')
                for row in self.result[group]['rows']}
        for block in ('a', 'b'):
            self.assertTrue(rows[block + '-negative-epsilon']['zero_increment'])
            self.assertFalse(rows[block + '-positive-epsilon']['zero_increment'])
            positive, negative = rows[block + '-positive-delta'], rows[block + '-negative-delta']
            self.assertEqual(positive['dcp_output_power_w'], .001)
            self.assertEqual(negative['dcp_output_power_w'], .001)
            self.assertGreater(positive['raw_output_power_w'], .001)
            self.assertLess(negative['raw_output_power_w'], .001)
            for point in (positive, negative):
                self.assertLess(point['hypotheses']['raw_continuous']['circular_error_deg'], 1e-8)

    def test_block_controls_completeness_and_offline_verification_scope(self):
        audit = self.result['input_identity_audit']
        self.assertEqual([row['control_profile'] for row in audit], ['a-zero'] * 5 + ['b-zero'] * 5)
        self.assertTrue(all(row['passed'] for row in audit))
        self.assertEqual([row['profile'] for row in self.result['zero_baseline_controls']], ['a-zero', 'b-zero'])
        self.assertTrue(all(row['all_comparisons_matched'] for row in self.result['zero_baseline_controls']))
        self.assertTrue(self.result['all_repeated_sweeps_matched'])
        self.assertTrue(self.result['complete_sequence_replication_matched'])
        self.assertEqual(self.result['sequence_status'], 'complete')
        self.assertEqual(self.result['missing_profile_count'], 0)
        self.assertEqual(self.result['order_comparisons']['completed_pair_count'], 5)
        self.assertEqual(self.result['order_comparisons']['missing_pair_count'], 0)
        self.assertEqual(self.result['historical_comparisons']['completed_count'], 6)
        self.assertTrue(self.result['historical_comparisons']['all_completed_comparisons_matched'])
        self.assertEqual(self.result['observed_sweep_point_count'], 610)
        self.assertEqual(sum(len(row['additional_sweep_points']) for row in self.result['repeated_quick_sweeps']), 10)
        self.assertFalse(self.result['quick_sweeps_are_independent_prediction_checks'])
        self.assertTrue(self.result['acquisition_archive_verified'])
        self.assertFalse(self.result['raw_sources_verified'])
        verification = self.result['acquisition_verification']
        self.assertFalse(verification['raw_source_bytes_verified'])
        self.assertEqual(verification['parameter_readbacks_per_capture'], 18)
        self.assertEqual(verification['restored_parameter_occurrences'], 247)
        self.assertEqual(self.result['plan_sha256'], m.scorer.PLAN_SHA256)
        self.assertEqual(self.result['historical_reference_sha256'], m.scorer.REFERENCE['sha256'])

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
            destination = root / 'report.json'
            cmd = command(root, validation, destination)
            completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            original = destination.read_bytes()
            self.assertFalse(json.loads(original)['raw_sources_verified'])
            self.assertFalse((root / 'build-reference').exists())
            self.assertNotEqual(subprocess.run(cmd, capture_output=True, check=False).returncode, 0)
            self.assertEqual(destination.read_bytes(), original)
            missing_raw = root / 'missing-raw-report.json'
            rejected = subprocess.run(command(root, validation, missing_raw) + ['--raw-root', str(root)],
                                      capture_output=True, text=True, check=False)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse(missing_raw.exists())

    def test_cli_exit_depends_on_acquisition_gates_not_descriptive_scientific_matches(self):
        # These are control-flow fixtures derived from a real report, not observations.
        with tempfile.TemporaryDirectory() as temporary:
            for gate, expected in ((None, 0), ('all_observed_zero_baselines_matched', 1),
                                   ('all_repeated_sweeps_matched', 1), ('all_planned_profiles_present', 1)):
                result = copy.deepcopy(self.result)
                result['complete_sequence_replication_matched'] = False
                result['historical_comparisons']['all_completed_comparisons_matched'] = False
                result['order_comparisons']['all_completed_pairs_matched'] = False
                if gate is not None:
                    result[gate] = False
                destination = Path(temporary) / (str(gate) + '.json')
                args = [str(SCRIPT), str(PLAN), str(OBSERVATIONS), str(destination),
                        '--capture-manifest', str(MANIFEST), '--root', str(ROOT)]
                with self.subTest(gate=gate), mock.patch.object(m, 'report', return_value=result), \
                        mock.patch.object(sys, 'argv', args), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(m.main(), expected)
                self.assertFalse(json.loads(destination.read_bytes())['complete_sequence_replication_matched'])

    def test_changed_plan_reference_or_observation_manifest_link_blocks_output(self):
        for kind in ('plan', 'reference', 'observation-link'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                validation = isolated_inputs(root)
                if kind in ('plan', 'reference'):
                    target = validation / (PLAN.name if kind == 'plan' else REFERENCE.name)
                    target.write_bytes(target.read_bytes() + b'\n')
                else:
                    target = validation / OBSERVATIONS.name
                    observations = json.loads(target.read_bytes())
                    observations['capture_manifest_sha256'] = '0' * 64
                    target.write_text(json.dumps(observations), encoding='utf-8')
                output = root / 'rejected.json'
                rejected = subprocess.run(command(root, validation, output), capture_output=True, text=True, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
