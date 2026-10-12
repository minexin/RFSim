"""Native planning, JSON remapping, independent wave checks and CLI atomicity."""
import cmath
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
INSTALLED = '--installed' in sys.argv
if INSTALLED:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(ROOT / 'python'))
import rfmodel
from rfmodel import Library, RFModelError
from rfmodel.conversion_network_file import analyze_conversion_network


def number(value):
    return complex(*value) if isinstance(value, list) else complex(value)


def channel_key(channel):
    return channel['device'], channel['port'], channel['bin']


class FrequencyPlanningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = Library(LIBRARY_PATH)

    def example(self):
        return json.loads((ROOT / 'examples/auto-frequency-mixer-polynomial.json').read_text(encoding='utf-8-sig'))

    def analyze(self, document):
        original = copy.deepcopy(document)
        result = analyze_conversion_network(self.lib, document)
        self.assertEqual(original, document)
        return result

    def equivalent(self, actual, expected):
        aa = {channel_key(c): i for i, c in enumerate(actual['channels'])}
        ee = {channel_key(c): i for i, c in enumerate(expected['channels'])}
        self.assertEqual(aa.keys(), ee.keys())
        for key, i in aa.items():
            j = ee[key]
            for field in ('incident', 'outgoing'):
                self.assertLess(abs(number(actual['channels'][i][field]) -
                                    number(expected['channels'][j][field])), 1e-11)
            for other, k in aa.items():
                l = ee[other]
                for field in ('noise_covariance_w_per_hz', 'noise_complementary_w_per_hz'):
                    a, b = number(actual[field][i][k]), number(expected[field][j][l])
                    self.assertLessEqual(abs(a-b), 1e-22 + 1e-9*abs(b))

    def test_native_rules_and_seed_permutation(self):
        plan = self.lib.plan_conversion_frequencies(1e6, [dict(
            ports=[2, 0, 1], seeds=[(1, 3), (0, 5)],
            rules=[dict(type='mixer', rf_port=0, lo_port=1, if_port=2)])])
        self.assertIsInstance(plan, rfmodel.ConversionFrequencyPlan)
        self.assertEqual(plan.channels, (((0, 5), (1, 3), (2, 2), (2, 8)),))
        self.assertEqual(plan.seed_channel_indices, ((1, 0),))
        self.assertEqual((plan.total_channels, plan.generated_channels), (4, 2))
        constant = self.lib.plan_conversion_frequencies(1, [dict(
            ports=[0, 1], rules=[dict(type='polynomial', input_port=0, output_port=1,
                                      voltage_coefficients=[1])])], [((0, 0), (0, 1))])
        self.assertEqual(constant.channels, (((0, 0), (1, 0)),))
        self.assertEqual(constant.seed_channel_indices, ((),))

    def test_json_multistage_equals_explicit_and_independent_time_projection(self):
        document = self.example()
        result = self.analyze(document)
        report = result['frequency_plan']
        self.assertEqual((report['total_channels'], report['generated_channels']), (24, 19))
        self.assertEqual(len(report['generated_boundary_channels']), 9)
        explicit = copy.deepcopy(document)
        del explicit['frequency_plan']
        explicit['devices'][1]['channels'] = [dict(port=p, bin=b) for p, bins in
                                               [(0, [2, 3]), (1, [1]), (2, [1, 2, 3, 4])]
                                               for b in bins]
        explicit['devices'][2]['channels'] = [dict(port=p, bin=b) for p, bins in
                                               [(0, [1, 2, 3, 4]), (1, range(9))] for b in bins]
        explicit['boundaries'].extend(dict(channel=['polynomial', 1, b], reflection=0,
                                            noise_temperature_k=290) for b in range(9))
        self.equivalent(result, self.analyze(explicit))
        for channel in result['channels']:
            if channel['device'] != 'polynomial' or channel['port'] != 1:
                continue
            b = channel['bin']
            total = 0j
            for i in range(256):
                angle = 2*math.pi*i/256
                rf = math.sqrt(2)*(.8*(.01+.002j)*cmath.exp(2j*angle) +
                                   .8*(.02-.003j)*cmath.exp(3j*angle)).real
                lo = math.sqrt(2)*((.1+.02j)*cmath.exp(1j*angle)).real
                voltage = math.sqrt(100)*rf*lo
                wave = (.2 + voltage + .1*voltage**2)/math.sqrt(50)
                total += wave*cmath.exp(-1j*b*angle)/256
            if b:
                total *= math.sqrt(2)
            self.assertLess(abs(number(channel['outgoing'])-total), 1e-12)

    def seed_document(self):
        return dict(format='rfmodel.conversion-network', version=1, spacing_hz=1e6,
                    frequency_plan=dict(new_channel_boundary=dict(reflection=0)),
                    operating_point=dict(initial_incident=[[.1, .02], [.2, .03], 0]),
                    devices=[dict(id='m', channels=[dict(port=1, bin=3), dict(port=0, bin=5),
                                                   dict(port=2, bin=8)],
                                  model=dict(type='bilinear_real_mixer', lo_reference_amplitude=1),
                                  noise=dict(covariance=[[0, 0, 0], [0, 0, 0], [0, 0, 3e-11]],
                                             complementary=[[0,0,0],[0,0,0],[0,0,3e-12]]),
                                  source_noise=dict(covariance=[[3e-10, [1e-10,2e-11], 0],
                                                                [[1e-10,-2e-11], 2e-10, 0], [0, 0, 0]],
                                                    complementary=[[1e-11,2e-12,0],[2e-12,-2e-11,0],[0,0,0]]),
                                  output_offset=[0, 0, [.01, .005]],
                                  am_pm=dict(peak_degrees_per_db=-5, output_reference_dbm=10,
                                             output_channels=[2]))],
                    additional_source_noise=dict(covariance=[[1e-11, 0, 0], [0, 2e-11, 0], [0, 0, 0]]),
                    boundaries=[dict(channel=['m', 1, 3], source=[.1, .02]),
                                dict(channel=['m', 0, 5], source=[.2, .03]),
                                dict(channel=['m', 2, 8])])

    def test_all_seed_indexed_quantities_and_am_pm_are_remapped(self):
        document = self.seed_document()
        result = self.analyze(document)
        explicit = copy.deepcopy(document)
        del explicit['frequency_plan']
        entry = explicit['devices'][0]
        entry['channels'] = [dict(port=p, bin=b) for p, b in [(0, 5), (1, 3), (2, 2), (2, 8)]]
        entry['noise'] = dict(covariance=[[0,0,0,0], [0,0,0,0], [0,0,0,0], [0,0,0,3e-11]],
                              complementary=[[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,3e-12]])
        entry['source_noise'] = dict(covariance=[[2e-10,[1e-10,-2e-11],0,0], [[1e-10,2e-11],3e-10,0,0],
                                                [0,0,0,0], [0,0,0,0]],
                                     complementary=[[-2e-11,2e-12,0,0],[2e-12,1e-11,0,0],
                                                    [0,0,0,0],[0,0,0,0]])
        entry['output_offset'] = [0,0,0,[.01,.005]]
        entry['am_pm']['output_channels'] = [3]
        explicit['additional_source_noise'] = dict(covariance=[[2e-11,0,0,0], [0,1e-11,0,0],
                                                               [0,0,0,0], [0,0,0,0]])
        explicit['operating_point']['initial_incident'] = [[.2,.03], [.1,.02], 0, 0]
        explicit['boundaries'].append(dict(channel=['m',2,2], reflection=0))
        self.equivalent(result, self.analyze(explicit))
        self.assertEqual(result['frequency_plan']['devices'][0]['seed_channel_indices'], [1,0,3])
        self.assertEqual(result['operating_point']['iterations'], 0)
        direct_difference = (.2+.03j)*(.1-.02j)
        self.assertLess(abs(number(result['channels'][2]['outgoing'])-direct_difference), 1e-12)

    def test_linear_positional_source_noise_and_offsets_use_original_bin_major_order(self):
        document = dict(format='rfmodel.conversion-network', version=1, spacing_hz=1e6,
                        frequency_plan={}, devices=[dict(id='l', bins=[1,2],
                        model=dict(type='linear', s=[[0,.5],[.5,0]]), noise=dict(noiseless=True),
                        source_noise=dict(covariance=[[1e-12,0,0,0],[0,0,0,0],
                                                       [0,0,3e-12,0],[0,0,0,0]]),
                        output_offset=[0,.1,0,.2])],
                        boundaries=[dict(channel=['l',p,b], source=.3 if p==0 else 0)
                                    for b in [1,2] for p in [0,1]])
        result = self.analyze(document)
        explicit = copy.deepcopy(document)
        del explicit['frequency_plan']
        self.equivalent(result,self.analyze(explicit))
        self.assertEqual(result['frequency_plan']['devices'][0]['seed_channel_indices'], [0,2,1,3])

    def test_generated_boundary_never_fills_missing_seed_boundary(self):
        document = self.example()
        document['boundaries'].pop(0)
        with self.assertRaisesRegex(ValueError, 'Every unconnected channel'):
            self.analyze(document)
        document = self.example()
        del document['frequency_plan']['new_channel_boundary']
        with self.assertRaisesRegex(ValueError, 'Every unconnected channel'):
            self.analyze(document)

    def test_explicit_generated_boundary_overrides_default(self):
        document = self.example()
        document['boundaries'].append(dict(channel=['polynomial',1,8], reflection=.2,
                                           noise_w_per_hz=3e-12))
        result = self.analyze(document)
        self.assertEqual(len(result['frequency_plan']['generated_boundary_channels']), 8)
        output = next(c for c in result['channels'] if channel_key(c)==('polynomial',1,8))
        self.assertGreater(output['incident_noise_w_per_hz'], 2.9e-12)

    def test_parameterized_linear_ports_are_explicit_and_checked(self):
        document = self.example()
        entry = document['devices'][0]
        entry['model'] = dict(type='linear', device=dict(type='matched_transmission', loss_db=2))
        with self.assertRaisesRegex(ValueError, 'frequency_ports'):
            self.analyze(document)
        entry['frequency_ports'] = 2
        self.assertEqual(self.analyze(document)['frequency_plan']['total_channels'], 24)
        entry['frequency_ports'] = 3
        with self.assertRaisesRegex(ValueError, 'Actual model ports differ'):
            self.analyze(document)
        # A static declaration mismatch is rejected before native model assembly.
        entry['model'] = dict(type='linear', s=[[0,.8],[.8,0]])
        entry['frequency_ports'] = 3
        with self.assertRaises(ValueError):
            self.analyze(document)
        entry['frequency_ports'] = 2
        self.assertEqual(self.analyze(document)['frequency_plan']['total_channels'],24)

    def test_unknown_model_rule_and_no_operating_point_rejected(self):
        document = self.example()
        document['devices'][1]['model']['type'] = 'unregistered_mixer'
        with self.assertRaisesRegex(ValueError, 'no automatic frequency rule'):
            self.analyze(document)
        document = self.example()
        del document['operating_point']
        with self.assertRaisesRegex(ValueError, 'operating_point'):
            self.analyze(document)

    def test_invalid_parameters_and_fixed_point_resource_failure(self):
        device = dict(ports=[0,1],seeds=[(0,1)],rules=[dict(type='polynomial',input_port=0,
                      output_port=1,voltage_coefficients=[0,0,1])])
        with self.assertRaises(RFModelError):
            self.lib.plan_conversion_frequencies(1,[device],[((0,0),(0,1))],max_bin=12)
        for value in [True, -1, 2147483648, 1.5]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.lib.plan_conversion_frequencies(1,[device],max_bin=value)
        for change in [dict(ports=[0,0]),dict(seeds=[(0,True)]),dict(rules=[dict(type='copy',
                       input_port=0,output_port=1,ignored=True)])]:
            malformed = dict(device,**change)
            with self.assertRaises((ValueError,RFModelError)):
                self.lib.plan_conversion_frequencies(1,[malformed])
        document = self.seed_document()
        document['devices'][0]['source_noise']['covariance'] = [[1,0],[0,1]]
        with self.assertRaises(ValueError):
            self.analyze(document)
        document = self.example()
        document['frequency_plan']['new_channel_boundary'] = dict(source=1)
        with self.assertRaises(ValueError):
            self.analyze(document)

    def test_cli_result_and_failed_planning_preserve_old_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory)/'input.json', Path(directory)/'output.json'
            document = self.example()
            source.write_text(json.dumps(document))
            env = dict(os.environ)
            if not INSTALLED:
                env['PYTHONPATH'] = str(ROOT/'python')
            command=[sys.executable,'-m','rfmodel',str(source),'--library',LIBRARY_PATH,
                     '--output',str(output)]
            success=subprocess.run(command,capture_output=True,text=True,env=env)
            self.assertEqual(success.returncode,0,success.stderr)
            saved=output.read_bytes()
            self.assertEqual(json.loads(saved)['frequency_plan']['total_channels'],24)
            document['frequency_plan']['max_bin']=7
            source.write_text(json.dumps(document))
            failed=subprocess.run(command,capture_output=True,text=True,env=env)
            self.assertNotEqual(failed.returncode,0)
            self.assertEqual(output.read_bytes(),saved)


if __name__ == '__main__':
    unittest.main()
