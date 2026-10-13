"""Independent synthetic acquisition guards; no SystemVue or native probe required."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('archive-am-pm-datapoints-zero-study.py')
_spec = importlib.util.spec_from_file_location('zero_archive', SCRIPT)
m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m)


def raw_fixture(extra=0):
    """Build hand-selected values without calling the normalizer's SI conversion."""
    profile = dict(name='synthetic-zero', power_dbm=[0, 10, 20], phase_deg=[10, 40, -20],
                   gain_db=20, frequency_hz=100000000, source_phase_deg=30,
                   output_p1db_dbm=100, output_saturation_dbm=103,
                   output_ip2_dbm=140, output_ip3_dbm=130, ordinary_output_target_dbm=0,
                   sweep_input_start_dbm=-21, sweep_input_stop_dbm=-19,
                   sweep_step_db=1, expected_points=3,
                   ordinary_nominal_predictions_degrees={'unscored_test_prediction': 40})
    nodes = []

    def add(path, data=None, dimensions=None, timestamp=None):
        nodes.append(dict(path=path, data=data, dimensions=dimensions, timestamp=timestamp))

    def vector(path, values):
        add(path, values, [len(values)])

    scalars = {
        m.AMP + 'AMtoPM_Mode': 2, m.AMP + 'G': 100,
        m.AMP + 'OP1dB': 10000000, m.AMP + 'OPSAT': 19952623.149688788,
        m.AMP + 'OIP2': 100000000000, m.AMP + 'OIP3': 10000000000,
        m.SOURCE + 'Pwr': .00001, m.SOURCE + 'Phase': math.pi / 6, m.SOURCE + 'Freq': 100000000,
        m.SWEEP + 'StartPwr': .000007943282347242822,
        m.SWEEP + 'StopPwr': .000012589254117941661,
        m.SWEEP + 'StepPwr': 1.2589254117941673, m.SWEEP + 'AutoLimits': 0,
        m.SWEEP + 'Freq': 100000000, m.SWEEP + 'SweepType': 3,
    }
    for path, value in scalars.items():
        add(path, value)
    add(m.AMP + 'PwrAMtoPMpoints', [.001, .01, .1], [1, 3])
    add(m.AMP + 'AMtoPMpoints', [math.pi / 18, 2 * math.pi / 9, -math.pi / 9], [1, 3])
    add(m.ANALYSIS)
    for path in (m.PATH + '1', m.PATH + '2', m.RAW):
        add(path, timestamp='1767225601')
    key = m.PATH + '1/Eqns/VarBlock/'
    vector(key + 'NodeNames', ['1', '2'])
    vector(key + 'CF', [100000000, 100000000])
    vector(key + 'DCP', [.00001, .001])
    vector(key + 'DCPH', [math.pi / 6, math.pi / 3])
    key = m.PATH + '2/Eqns/VarBlock/'
    count = 3 + extra
    inputs = [.000007943282347242822, .00001, .000012589254117941661,
              .00001584893192461114, .000019952623149688788]
    vector(key + 'NodeNames', ['1', '2'] * count)
    vector(key + 'CF', [100000000] * (2 * count))
    vector(key + 'DCP', [value for i in range(count) for value in (inputs[i], .0009 + i * .0001)])
    vector(key + 'DCPH', [value for i in range(count) for value in (0, .1 * (i + 1))])
    key = m.RAW + '/Eqns/VarBlock/'
    vector(key + 'IDNo', [7, 9])
    vector(key + 'IDName', ['{1}D[100000000],Source,RFAmp', 'unrelated background'])
    vector(key + 'ID2', [7, 9, 7])
    vector(key + 'F2', [99999999.5, 100000002, 100000000.5])
    vector(key + 'P2', [.00125, 0, .00125])
    voltage = [math.sqrt(.125) / 2, math.sqrt(.125) * math.sqrt(3) / 2]
    vector(key + 'V2', voltage + [0, 0] + voltage)
    vector(key + 'Z2', [50, 0, 50, 0, 50, 0])
    capture = dict(profile=profile['name'], analysis_target=m.ANALYSIS, analysis_done=1,
                   manager_errors='', run_started_utc='2026-01-01T00:00:00.1234567Z',
                   run_returned_utc='2026-01-01T00:00:02.2345678Z', nodes=nodes)
    return capture, profile


def node(capture, path):
    return next(item for item in capture['nodes'] if item['path'] == path)


class DataPointsZeroArchiveTests(unittest.TestCase):
    def setUp(self):
        self.capture, self.profile = raw_fixture()

    def reject(self, mutation):
        capture = copy.deepcopy(self.capture)
        mutation(capture)
        with self.assertRaises(ValueError):
            m.normalize(capture, self.profile)

    def test_expected_si_parameters_match_independent_readbacks(self):
        expected = m.expected_parameters(self.profile)
        self.assertEqual(len(expected), 17)
        for path, wanted in expected.items():
            actual = node(self.capture, path)['data']
            if isinstance(wanted, list):
                self.assertEqual(len(wanted), len(actual))
                for a, b in zip(actual, wanted):
                    self.assertAlmostEqual(a, b, delta=1e-12 * max(abs(b), 1e-12))
            else:
                self.assertAlmostEqual(actual, wanted, delta=1e-12 * max(abs(wanted), 1e-12))

    def test_axes_and_full_profile_are_preserved_without_input_mutation(self):
        original = copy.deepcopy((self.capture, self.profile))
        result = m.normalize(self.capture, self.profile)
        self.assertEqual(result['profile'], self.profile)
        self.assertEqual((self.capture, self.profile), original)
        self.assertEqual(result['ordinary']['index'], 0)
        self.assertEqual(result['ordinary']['output_power_w'], .001)
        self.assertEqual(result['ordinary']['raw_output_w'], .00125)
        self.assertAlmostEqual(result['ordinary']['raw_vs_report_relative_error'], .2)
        self.assertEqual(len(result['ordinary']['raw_carrier']), 2)
        self.assertEqual(result['sweep'][0]['nominal_output_dbm'], -1)
        self.assertEqual(result['sweep'][0]['output_power_w'], .0009)
        self.assertEqual(result['additional_sweep_points'], [])
        self.assertEqual(result['analysis_target'], m.ANALYSIS)
        self.assertEqual(set(result['timestamps']), {m.PATH + '1', m.PATH + '2', m.RAW})
        result['profile']['phase_deg'][0] = 999
        self.assertEqual(self.profile['phase_deg'][0], 10)

    def test_one_extra_endpoint_is_retained_and_still_validated(self):
        capture, profile = raw_fixture(extra=1)
        result = m.normalize(capture, profile)
        self.assertEqual([row['index'] for row in result['sweep']], [0, 1, 2])
        self.assertEqual(len(result['additional_sweep_points']), 1)
        extra = result['additional_sweep_points'][0]
        self.assertEqual(extra['index'], 3)
        self.assertEqual(extra['nominal_output_dbm'], 2)
        node(capture, m.PATH + '2/Eqns/VarBlock/DCP')['data'][-2] = .00001
        with self.assertRaisesRegex(ValueError, 'Sweep grid'):
            m.normalize(capture, profile)

    def test_two_extra_endpoints_rejected(self):
        capture, profile = raw_fixture(extra=2)
        with self.assertRaisesRegex(ValueError, 'sweep size'):
            m.normalize(capture, profile)

    def test_unrelated_dc_spectrum_is_retained_without_becoming_the_carrier(self):
        node(self.capture, m.RAW + '/Eqns/VarBlock/F2')['data'][1] = 0
        result = m.normalize(self.capture, self.profile)
        self.assertEqual([carrier['frequency_hz'] for carrier in result['ordinary']['raw_carrier']],
                         [99999999.5, 100000000.5])

    def test_registered_dense_grid_preserves_exact_decimal_targets_and_extra(self):
        plan_path = SCRIPT.parents[2] / 'validation/am-pm-datapoints-zero-plan-20261013.json'
        plan = json.loads(plan_path.read_text(encoding='utf-8-sig'))
        profile = next(p for p in plan['profiles'] if p['name'] == 'dense-zero-neighborhood')
        capture = copy.deepcopy(self.capture)
        capture['profile'] = profile['name']
        source_power = 10 ** ((profile['ordinary_output_target_dbm'] - profile['gain_db'] - 30) / 10)
        node(capture, m.SOURCE + 'Pwr')['data'] = source_power
        node(capture, m.SOURCE + 'Phase')['data'] = 0
        node(capture, m.SWEEP + 'StartPwr')['data'] = 10 ** ((profile['sweep_input_start_dbm'] - 30) / 10)
        node(capture, m.SWEEP + 'StopPwr')['data'] = 10 ** ((profile['sweep_input_stop_dbm'] - 30) / 10)
        node(capture, m.SWEEP + 'StepPwr')['data'] = 10 ** (profile['sweep_step_db'] / 10)
        node(capture, m.PATH + '1/Eqns/VarBlock/DCP')['data'][0] = source_power
        node(capture, m.PATH + '1/Eqns/VarBlock/DCPH')['data'][0] = 0
        targets = [prediction['nominal_output_dbm'] for prediction in profile['sweep_nominal_predictions']]
        # One extra endpoint is checked independently of the registered grid.
        all_targets = targets + [.011]
        inputs = [10 ** ((target - profile['gain_db'] - 30) / 10) for target in all_targets]
        values = {
            'NodeNames': ['1', '2'] * len(inputs),
            'CF': [profile['frequency_hz']] * (2 * len(inputs)),
            'DCP': [value for power in inputs for value in (power, .001)],
            'DCPH': [0, .1] * len(inputs),
        }
        for name, data in values.items():
            node(capture, m.PATH + '2/Eqns/VarBlock/' + name).update(data=data, dimensions=[len(data)])
        result = m.normalize(capture, profile)
        self.assertEqual([row['nominal_output_dbm'] for row in result['sweep']], targets)
        self.assertEqual(result['sweep'][0]['nominal_output_dbm'], -.01)
        self.assertEqual(result['sweep'][10]['nominal_output_dbm'], 0)
        self.assertEqual(result['sweep'][20]['nominal_output_dbm'], .01)
        self.assertEqual(result['additional_sweep_points'][0]['nominal_output_dbm'], .011)

    def test_metadata_guards(self):
        for field, values in {
            'profile': ['other', None],
            'analysis_target': [m.BASE + 'System1', 'RFModel_AMPM_A01/PointDerivative/System1', None],
            'analysis_done': [0, 2, True, '1', None],
            'manager_errors': ['(INFO) changed setup', '(WARNING) simulation warning', None],
            'run_started_utc': ['2026-01-01T00:00:03Z', '2026-01-01T00:00:00'],
        }.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    self.reject(lambda c: c.update({field: value}))

    def test_all_three_datasets_must_be_fresh(self):
        for path in (m.PATH + '1', m.PATH + '2', m.RAW):
            for stamp in ['0', '1767225610', 'NaN', True, 1767225601.5]:
                with self.subTest(path=path, stamp=stamp):
                    self.reject(lambda c: node(c, path).update(timestamp=stamp))

    def test_missing_duplicate_or_wrong_target_nodes_rejected(self):
        for path in (m.ANALYSIS, m.AMP + 'PwrAMtoPMpoints', m.PATH + '2', m.RAW):
            with self.subTest(path=path, kind='missing'):
                self.reject(lambda c: c['nodes'].remove(node(c, path)))
            with self.subTest(path=path, kind='duplicate'):
                self.reject(lambda c: c['nodes'].append(copy.deepcopy(node(c, path))))
        self.reject(lambda c: node(c, m.ANALYSIS).update(path='RFModel_AMPM_A01/PointDerivative/System1'))

    def test_literal_table_shape_and_si_values_are_checked(self):
        for path in (m.AMP + 'PwrAMtoPMpoints', m.AMP + 'AMtoPMpoints'):
            for shape in ([3], [3, 1], [1, 2], None):
                with self.subTest(path=path, shape=shape):
                    self.reject(lambda c: node(c, path).update(dimensions=shape))
            for value in (math.nan, math.inf, True, '0.1'):
                with self.subTest(path=path, value=value):
                    self.reject(lambda c: node(c, path)['data'].__setitem__(0, value))
            self.reject(lambda c: node(c, path)['data'].pop())
        self.reject(lambda c: node(c, m.AMP + 'PwrAMtoPMpoints').update(data=[0, 10, 20]))
        self.reject(lambda c: node(c, m.AMP + 'AMtoPMpoints').update(data=[10, 40, -20]))

    def test_all_scalar_readbacks_are_checked(self):
        for path, wanted in m.expected_parameters(self.profile).items():
            if isinstance(wanted, list):
                continue
            for value in (wanted + max(1, abs(wanted)) * .001, math.nan, math.inf, True, str(wanted)):
                with self.subTest(path=path, value=value):
                    self.reject(lambda c: node(c, path).update(data=value))
        self.reject(lambda c: node(c, m.SOURCE + 'Pwr').update(dimensions=[1]))

    def test_channel_shapes_frequencies_and_phases_are_checked(self):
        for suffix in ('1', '2'):
            key = m.PATH + suffix + '/Eqns/VarBlock/'
            for field in ('NodeNames', 'CF', 'DCP', 'DCPH'):
                with self.subTest(suffix=suffix, field=field):
                    self.reject(lambda c: node(c, key + field).update(dimensions=[1, 2]))
            self.reject(lambda c: node(c, key + 'NodeNames')['data'].__setitem__(0, '2'))
            self.reject(lambda c: node(c, key + 'CF')['data'].__setitem__(0, 915000000))
            self.reject(lambda c: node(c, key + 'DCP')['data'].__setitem__(0, 0))
            self.reject(lambda c: node(c, key + 'DCPH')['data'].__setitem__(1, math.nan))
        self.reject(lambda c: node(c, m.PATH + '1/Eqns/VarBlock/DCPH')['data'].__setitem__(0, 0))
        self.reject(lambda c: node(c, m.PATH + '2/Eqns/VarBlock/DCPH')['data'].__setitem__(0, math.pi / 6))
        self.reject(lambda c: node(c, m.PATH + '1/Eqns/VarBlock/DCP')['data'].__setitem__(0, .001))
        self.reject(lambda c: node(c, m.PATH + '2/Eqns/VarBlock/DCP')['data'].__setitem__(2, .00002))

    def test_grid_cannot_drop_duplicate_or_reorder_planned_points(self):
        key = m.PATH + '2/Eqns/VarBlock/'
        def shorten(capture):
            for field in ('NodeNames', 'CF', 'DCP', 'DCPH'):
                item = node(capture, key + field)
                item['data'] = item['data'][:-2]
                item['dimensions'] = [len(item['data'])]
        self.reject(shorten)
        def exchange(capture):
            values = node(capture, key + 'DCP')['data']
            values[0], values[2] = values[2], values[0]
        self.reject(exchange)

    def test_raw_identity_support_shape_and_finiteness_guards(self):
        key = m.RAW + '/Eqns/VarBlock/'
        changes = [
            ('IDNo', [7, 7]), ('IDName', ['unrelated', 'also unrelated']),
            ('IDName', ['{1}D[100000000],Source,RFAmp', '{1}D[200000000],Source,RFAmp']),
            ('ID2', [7, 999, 7]), ('F2', [99999999.5, 100000002, 100000001.5]),
            ('P2', [.00125, -1, .00125]), ('P2', [.00125, math.nan, .00125]),
            ('Z2', [0, 0, 50, 0, 50, 0]),
            ('V2', [1, 0, math.nan, 0, 1, 0]),
        ]
        for field, values in changes:
            with self.subTest(field=field, values=values):
                self.reject(lambda c: node(c, key + field).update(data=values))
        self.reject(lambda c: node(c, key + 'V2').update(dimensions=[3, 2]))
        self.reject(lambda c: node(c, key + 'Z2')['data'].pop())

    def test_raw_power_and_phase_inconsistencies_are_rejected(self):
        key = m.RAW + '/Eqns/VarBlock/'
        self.reject(lambda c: node(c, key + 'P2')['data'].__setitem__(0, .001))
        def rotate(capture):
            values = node(capture, key + 'V2')['data']
            values[0], values[1] = -values[1], values[0]
        self.reject(rotate)
        def unequal_carriers(capture):
            node(capture, key + 'P2')['data'][2] *= 4
            node(capture, key + 'V2')['data'][4] *= 2
            node(capture, key + 'V2')['data'][5] *= 2
        self.reject(unequal_carriers)

    def test_invalid_profile_cannot_define_expected_values(self):
        for field, values in {
            'power_dbm': [[0, 0, 20], [0, 10], [0, math.nan, 20]],
            'phase_deg': [[10, True, -20], [10, math.inf, -20]],
            'expected_points': [True, 2, 4],
            'sweep_step_db': [0, -1, .5],
            'frequency_hz': [0, math.nan],
            'gain_db': [math.inf, 100000],
        }.items():
            for value in values:
                profile = copy.deepcopy(self.profile)
                profile[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    m.expected_parameters(profile)

    def test_cli_normalizes_one_capture_and_refuses_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            source, plan, output = (folder / name for name in ('capture.json', 'plan.json', 'output.json'))
            source.write_text(json.dumps(self.capture), encoding='utf-8')
            plan.write_text(json.dumps({'profiles': [self.profile]}), encoding='utf-8')
            command = [sys.executable, '-I', '-S', str(SCRIPT), str(source), str(plan), self.profile['name'], str(output)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(json.loads(output.read_text()), m.normalize(self.capture, self.profile))
            output.write_bytes(b'preserve')
            second = subprocess.run(command, capture_output=True)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(output.read_bytes(), b'preserve')

    def test_cli_rejects_duplicate_profile_without_creating_output(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            source, plan, output = (folder / name for name in ('capture.json', 'plan.json', 'output.json'))
            source.write_text(json.dumps(self.capture), encoding='utf-8')
            plan.write_text(json.dumps({'profiles': [self.profile, self.profile]}), encoding='utf-8')
            result = subprocess.run([sys.executable, '-I', '-S', str(SCRIPT), str(source), str(plan),
                                     self.profile['name'], str(output)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
