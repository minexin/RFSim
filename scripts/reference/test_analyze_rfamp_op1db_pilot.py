"""Acquisition identity, measured input and holdout separation for the OP1dB pilot."""
import copy
import importlib.util
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_replay import assert_no_product_imports, suite_provider

PROVIDER = suite_provider(sys.argv)
ROOT = Path(__file__).resolve().parents[2]
LOCAL = None
if '--local-runs' in sys.argv:
    index = sys.argv.index('--local-runs')
    LOCAL = Path(sys.argv[index + 1]).resolve()
    del sys.argv[index:index + 2]
spec = importlib.util.spec_from_file_location('op1db_analysis', Path(__file__).with_name('analyze-rfamp-op1db-pilot.py'))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class PilotAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = PROVIDER()
        if LOCAL:
            cls.a = analysis.read_run(LOCAL / 'A-baseline', 'A-baseline', LOCAL.parent / 'prepared')
            cls.b = analysis.read_run(LOCAL / 'B-op1db21', 'B-op1db21', LOCAL.parent / 'prepared')
        else:
            packet = analysis.read_json(ROOT / 'validation/rfamp-op1db-pilot-ab-20261015.json')
            cls.a, cls.b = packet['runs']
        cls.fit_a = analysis.identify(cls.native, cls.a)
        cls.fit_b = analysis.identify(cls.native, cls.b)
        with tempfile.TemporaryDirectory() as directory:
            cls.frozen = analysis.freeze(cls.native, cls.a, cls.b, directory)

    def test_only_registered_parameter_changes(self):
        differences = analysis.validate_ab(self.a, self.b)
        self.assertEqual(len(differences), 2)
        self.assertEqual({d['field'] for d in differences}, {'Data', 'DataEntry'})
        self.assertEqual(self.fit_a['measured_input'], self.fit_b['measured_input'])

    def test_extra_parameter_change_rejected(self):
        b = copy.deepcopy(self.b)
        target = next(p for p in b['parameter_units']['readback']['parameters'] if p['path'].endswith('/RFAmp2/ParamSet/G'))
        next(v for v in target['variables'] if v['name'] == 'DataEntry')['value'] = '-9'
        with self.assertRaises(ValueError):
            analysis.validate_ab(self.a, b)

    def test_duplicate_parameter_or_unit_field_rejected(self):
        for duplicate_parameter in (True, False):
            units = copy.deepcopy(self.a['parameter_units'])
            items = units['readback']['parameters']
            if duplicate_parameter:
                items.append(copy.deepcopy(items[0]))
            else:
                items[0]['variables'].append(copy.deepcopy(items[0]['variables'][0]))
            with self.assertRaises(ValueError):
                analysis.units_index(units, self.a['capture'])

    def test_unit_and_live_value_mismatch_rejected(self):
        for field, value in (('Unit', 1), ('Data', .2), ('DataEntry', '21')):
            units = copy.deepcopy(self.a['parameter_units'])
            parameter = next(p for p in units['readback']['parameters'] if p['path'].endswith('/RFAmp1/ParamSet/OP1dB'))
            next(v for v in parameter['variables'] if v['name'] == field)['value'] = value
            with self.assertRaises(ValueError):
                analysis.units_index(units, self.a['capture'])

    def test_readback_time_mismatch_rejected(self):
        units = copy.deepcopy(self.a['parameter_units'])
        units['readback']['utc'] = '2000-01-01T00:00:00Z'
        with self.assertRaises(ValueError):
            analysis.units_index(units, self.a['capture'])

    def test_reversed_run_or_workspace_identity_rejected(self):
        with self.assertRaises(ValueError):
            analysis.validate_ab(self.b, self.a)
        for key, value in (('workspace_sha256', '0' * 64),
                           ('run_started_utc', self.a['capture']['run_started_utc'])):
            b = copy.deepcopy(self.b)
            b['capture'][key] = value
            with self.assertRaises(ValueError):
                analysis.validate_ab(self.a, b)

    def test_extra_submitted_control_rejected(self):
        b = copy.deepcopy(self.b)
        b['capture']['submitted_script'] += '\r\nCall unexpected()\r\n'
        with self.assertRaises(ValueError):
            analysis.validate_ab(self.a, b)

    def test_actual_input_has_no_nominal_substitute(self):
        self.assertNotEqual(self.fit_a['measured_input']['sources'][0]['nominal_wave'],
                            self.fit_a['measured_input']['sources'][0]['measured_voltage_wave'])
        a = copy.deepcopy(self.a)
        a['input_reference']['sources'] = []
        with self.assertRaises(ValueError):
            analysis.identify(self.native, a)

    def test_identification_is_not_a_holdout(self):
        for fit in (self.fit_a, self.fit_b):
            self.assertEqual(fit['independent_prediction_count'], 0)
            self.assertEqual(fit['operating_point']['nonlinear_input_scale'], 1)
            self.assertTrue(fit['common_fundamental_control']['passed'])
        a = copy.deepcopy(self.a)
        a['capture']['two_tone'] = True
        with self.assertRaises(ValueError):
            analysis.identify(self.native, a)

    def test_unchanged_coefficients_failure_is_retained(self):
        null = self.frozen['unchanged_coefficients_at_B']
        self.assertEqual(null['recorded_origin_count'], 18)
        self.assertEqual(null['passed_origin_count'], 0)
        self.assertEqual(null['failed_origin_count'], 18)
        self.assertGreater(null['max_relative_wave_error'], 3)
        self.assertFalse(null['complete_and_passed'])

    def test_prospective_input_is_explicitly_assumed(self):
        prospective = self.frozen['C_prospective_input_assumption']
        measured = complex(*self.fit_b['measured_input']['sources'][0]['measured_voltage_wave'])
        self.assertEqual(complex(*prospective[0]['wave']), measured)
        self.assertEqual(complex(*prospective[1]['wave']), 1j * measured)
        terms = self.frozen['C_prospective_prediction']['terms']
        self.assertEqual({order: sum(t['order'] == order for t in terms) for order in analysis.ORDERS},
                         {5: 28, 7: 60, 9: 110, 11: 182})
        self.assertEqual(len({t['label'] for t in terms}), 380)

    def test_synthetic_mismatch_missing_and_unexpected_are_not_passes(self):
        # Artificial observations for scorer mechanics only; never vendor evidence.
        prediction = {'terms': [{'order': 5, 'label': '[5x(Source.Source1)],RFAmp1',
                                 'bin': 50, 'bandwidth_hz': 5, 'expected_wave': [1., 0.]}]}
        exact = {prediction['terms'][0]['label']: [(5e9 - 2.5, 1+0j, 1), (5e9 + 2.5, 1+0j, 1)]}
        self.assertTrue(analysis.compare_terms(prediction, exact)['complete_and_passed'])
        failed = copy.deepcopy(exact)
        failed[next(iter(failed))][1] = (5e9 + 2.5, 1+2e-7j, 1)
        self.assertEqual(analysis.compare_terms(prediction, failed)['failed_origin_count'], 1)
        missing = analysis.compare_terms(prediction, {})
        self.assertEqual(missing['recorded_origin_count'], 0)
        self.assertEqual(len(missing['missing_origins']), 1)
        extra = dict(exact, **{'[7x(Source.Source1)],RFAmp1': [(7e9 - 3.5, 1+0j, 2), (7e9 + 3.5, 1+0j, 2)]})
        self.assertFalse(analysis.compare_terms(prediction, extra)['complete_and_passed'])

    def test_bad_bandwidth_is_not_a_numeric_failure(self):
        term = self.frozen['C_prospective_prediction']['terms'][0]
        with self.assertRaises(ValueError):
            analysis.compare_terms({'terms': [term]}, {term['label']: [(0., 1+0j, 1)]})

    def test_freeze_refuses_existing_C_or_D(self):
        for name in ('C-two-tone-holdout', 'D-restore-baseline'):
            with tempfile.TemporaryDirectory() as directory:
                (Path(directory) / name).mkdir()
                with self.assertRaises(ValueError):
                    analysis.freeze(self.native, self.a, self.b, directory)

    def test_frozen_contract_change_rejected_before_C_data(self):
        for key, value in (('relative_complex_wave_tolerance', 1e-5), ('plan_sha256', '0' * 64),
                           ('classification', 'changed'), ('logic_sha256_lf', {})):
            frozen = copy.deepcopy(self.frozen)
            frozen[key] = value
            with self.assertRaises(ValueError):
                analysis.score_c(self.native, frozen, {})

    def test_coefficient_domain_rejected(self):
        sources, _ = analysis.measured.measured_sources(self.b['capture'], self.b['input_reference'])
        for index, value in ((0, 1.), (5, 0.), (7, math.nan), (6, -.1)):
            coefficients = list(self.fit_b['coefficients_c0_to_c11'])
            coefficients[index] = value
            with self.assertRaises(ValueError):
                analysis.predict(self.native, self.b['capture'], sources, coefficients)

    def test_output_is_exclusive_and_no_product_package_loaded(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frozen.json'
            analysis.write_new(path, {'preserved': True})
            with self.assertRaises(FileExistsError):
                analysis.write_new(path, {'preserved': False})
            self.assertEqual(analysis.read_json(path), {'preserved': True})
        assert_no_product_imports()


if __name__ == '__main__':
    unittest.main()
