"""Completed holdout replay and evidence-tampering regression checks."""
import copy
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_replay import assert_no_product_imports, suite_provider

PROVIDER = suite_provider(sys.argv)
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('op1db_report', Path(__file__).with_name('report-rfamp-op1db-pilot.py'))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class CompletedPilotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = PROVIDER()
        cls.directory = ROOT / 'validation'
        read = lambda name: report.analysis.read_json(cls.directory / f'rfamp-op1db-pilot-{name}-20261015.json')
        cls.frozen = read('freeze')
        cls.packets = read('ab')['runs'] + [read('c')['run'], read('d')['run']]
        cls.result = report.replay(cls.native, cls.directory)

    def test_actual_holdout_and_exact_end_control(self):
        result = self.result
        self.assertTrue(result['passed'])
        primary = result['C']['measured_input_prediction']
        self.assertEqual(primary['expected_origin_count'], 380)
        self.assertEqual(primary['passed_origin_count'], 380)
        self.assertLess(primary['max_relative_wave_error'], 1e-7)
        self.assertEqual(sum(p['compared_band_edge_rows'] for p in result['D']['ports'].values()), 214)
        self.assertTrue(all(p['complex_waves_exact'] for p in result['D']['ports'].values()))
        self.assertEqual(result['D']['independent_prediction_count'], 0)
        self.assertFalse(result['full_RFAMP_compatibility'])

    def test_post_C_replay_never_fits_or_freezes(self):
        with patch.object(report.analysis, 'identify', side_effect=AssertionError('refit')), \
             patch.object(report.analysis, 'freeze', side_effect=AssertionError('refreeze')):
            self.assertTrue(report.replay(self.native, self.directory)['passed'])

    def test_independent_direct_array_audit(self):
        audit = self.result['independent_audit']
        self.assertEqual({k: v['origins'] for k, v in audit['C_by_order'].items()},
                         {'5': 28, '7': 60, '9': 110, '11': 182})
        self.assertEqual(audit['C_band_edge_rows'], 760)
        self.assertLess(audit['C_max_relative_wave_error'], 1e-7)
        self.assertEqual(audit['raw_runs_verified'], [])  # CI does not pretend to own local raw files.

    def test_audit_rejects_changed_original_wave_and_power(self):
        packets = copy.deepcopy(self.packets)
        capture = packets[2]['capture']
        voltages = report.independent.direct.array(capture, 'V3')
        powers = report.independent.direct.array(capture, 'P3')
        for i in range(len(voltages)):
            voltages[i] *= 1.001
        for i in range(len(powers)):
            powers[i] *= 1.001**2
        self.assertFalse(report.independent.audit(self.frozen, packets)['passed'])

    def test_cannot_ignore_missing_original_array_row(self):
        packets = copy.deepcopy(self.packets)
        frequencies = report.independent.direct.array(packets[2]['capture'], 'F3')
        for i in range(len(frequencies)):
            frequencies[i] += 1.
        # The physical support changed, rather than merely the numerical residual.
        with self.assertRaises(ValueError):
            report.independent.audit(self.frozen, packets)

    def test_frozen_bytes_are_mandatory(self):
        with tempfile.TemporaryDirectory() as name:
            temporary = Path(name)
            for source in self.directory.glob('rfamp-op1db-pilot-*-20261015.json'):
                shutil.copyfile(source, temporary / source.name)
            frozen = temporary / 'rfamp-op1db-pilot-freeze-20261015.json'
            frozen.write_bytes(frozen.read_bytes() + b' ')
            with self.assertRaisesRegex(ValueError, 'registration/freeze bytes'):
                report.replay(self.native, temporary)

    def test_observation_bytes_are_bound(self):
        with tempfile.TemporaryDirectory() as name:
            temporary = Path(name)
            for source in self.directory.glob('rfamp-op1db-pilot-*-20261015.json'):
                shutil.copyfile(source, temporary / source.name)
            source = temporary / 'rfamp-op1db-pilot-c-20261015.json'
            source.write_bytes(source.read_bytes() + b' ')
            with self.assertRaisesRegex(ValueError, 'evidence file changed'):
                report.replay(self.native, temporary)

    def test_D_parameter_drift_rejected(self):
        a, _, c, d = copy.deepcopy(self.packets)
        d['capture']['submitted_script'] += '\nchanged'
        with self.assertRaises(ValueError):
            report.analysis.score_d(a, c, d)

    def test_C_cannot_precede_freeze(self):
        c = copy.deepcopy(self.packets[2])
        c['capture']['run_started_utc'] = self.frozen['created_utc']
        with self.assertRaises(ValueError):
            report.analysis.score_c(self.native, self.frozen, c)

    def test_no_product_imports(self):
        assert_no_product_imports()


if __name__ == '__main__':
    unittest.main()
