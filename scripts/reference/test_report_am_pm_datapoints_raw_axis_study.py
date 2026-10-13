"""Real-data report regression; all fixtures are mandatory and no COM is used."""

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('report-am-pm-datapoints-raw-axis-study.py')
spec = importlib.util.spec_from_file_location('raw_axis_report', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
ROOT = SCRIPT.resolve().parents[2]
PLAN = ROOT/'validation/am-pm-datapoints-raw-axis-plan-20261013.json'
OBSERVATIONS = ROOT/'validation/am-pm-datapoints-raw-axis-observations-20261013.json'
MANIFEST = ROOT/'validation/am-pm-datapoints-raw-axis-captures-20261013.json'
DIAGNOSTIC = ROOT/'validation/am-pm-datapoints-raw-axis-diagnostic-20261013.json'


class ReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = m.report(ROOT, PLAN, OBSERVATIONS, MANIFEST)

    def test_frozen_failures_and_independent_control_denominators(self):
        for key, count in zip(m.scorer.HYPOTHESES, (5, 9, 9, 5)):
            summary = self.result['new_prediction_checks']['summary'][key]
            self.assertEqual((summary['point_count'], summary['passed_count'], summary['passed']), (10, count, False))
        for key, count in zip(m.scorer.HYPOTHESES, (0, 0, 1, 1)):
            summary = self.result['reproduction_controls']['summary'][key]
            self.assertEqual((summary['point_count'], summary['passed_count']), (1, count))
        self.assertFalse(self.result['eligible_for_vendor_compatibility'])
        self.assertFalse(self.result['product_formula_changed'])

    def test_nonzero_target_counterexample_and_report_plateau_retained(self):
        self.assertEqual(self.result['observed_zero_increment_profiles'], ['control-zero', 'negative-0p000001'])
        rows = {row['profile']: row for row in self.result['new_prediction_checks']['rows']}
        negative, positive = rows['negative-0p000001'], rows['positive-0p000001']
        self.assertEqual(negative['nominal_output_dbm'], -1e-6)
        self.assertEqual(negative['observed_increment_deg'], 0)
        self.assertLess(negative['raw_output_power_w'], .001)
        self.assertGreater(positive['raw_output_power_w'], .001)
        self.assertGreater(positive['observed_increment_deg'], 33.43749)
        self.assertEqual(negative['dcp_output_power_w'], positive['dcp_output_power_w'])
        self.assertEqual(len(self.result['observed_exact_dcp_one_milliwatt_profiles']), 7)
        for name in ('raw_continuous', 'raw_nominal_zero_bypass'):
            failed = [r['profile'] for r in rows.values() if not r['hypotheses'][name]['passed']]
            self.assertEqual(failed, ['negative-0p000001'])

    def test_input_identity_controls_and_raw_verification_scope(self):
        audit = self.result['input_identity_audit']
        self.assertEqual(len(audit), 11)
        self.assertTrue(all(row['passed'] for row in audit))
        self.assertLess(max(row['relative_error'] for row in audit), 1e-13)
        self.assertEqual(self.result['baseline']['comparison_count'], 63)
        self.assertTrue(self.result['baseline']['all_comparisons_matched'])
        self.assertTrue(self.result['all_repeated_sweeps_matched'])
        self.assertEqual(self.result['observed_sweep_point_count'], 671)
        self.assertFalse(self.result['quick_sweeps_are_independent_prediction_checks'])
        self.assertTrue(self.result['acquisition_archive_verified'])
        self.assertFalse(self.result['raw_sources_verified'])
        self.assertFalse(self.result['acquisition_verification']['raw_source_bytes_verified'])

    def test_retained_raw_verified_diagnostic_matches_offline_replay(self):
        retained = m.scorer.load(DIAGNOSTIC)
        self.assertTrue(retained['raw_sources_verified'])
        self.assertTrue(retained['acquisition_verification']['raw_source_bytes_verified'])
        retained['raw_sources_verified'] = False
        retained['acquisition_verification']['raw_source_bytes_verified'] = False
        m.archive.compare_replay(retained, self.result)

    def test_cli_isolated_without_raw_and_refuses_overwrite_or_fake_raw_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            isolated = Path(temporary)
            validation = isolated/'validation'
            validation.mkdir()
            for source in (PLAN, OBSERVATIONS, MANIFEST,
                           ROOT/'validation/am-pm-datapoints-raw-axis-workflow-20261013.json',
                           ROOT/m.scorer.REFERENCE['path']):
                shutil.copyfile(source, validation/source.name)
            destination = isolated/'report.json'
            command = [sys.executable, '-I', '-S', '-B', str(SCRIPT), str(validation/PLAN.name),
                       str(validation/OBSERVATIONS.name), str(destination), '--capture-manifest',
                       str(validation/MANIFEST.name), '--root', str(isolated)]
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            original = destination.read_bytes()
            self.assertFalse(json.loads(original)['raw_sources_verified'])
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(destination.read_bytes(), original)
            command[7] = str(isolated/'unverified.json')
            completed = subprocess.run(command + ['--raw-root', str(isolated)], capture_output=True, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertFalse((isolated/'unverified.json').exists())


if __name__ == '__main__':
    unittest.main()
