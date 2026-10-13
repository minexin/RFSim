"""Offline input-carrier guards using a historical excerpt and synthetic faults.

The excerpt is retained old evidence, not a new observation. Synthetic captures
combine that input excerpt with the existing synthetic output/acquisition
fixture only to exercise the complete normalizer; they are never evidence.
"""

import copy
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('normalize-am-pm-datapoints-allocation.py')
ROOT = SCRIPT.resolve().parents[2]
FIXTURE = ROOT / 'validation/am-pm-datapoints-input-spectrum-reference-20261014.json'
FIXTURE_SHA256 = '7c7a4c6bee77173be551c2c1c7799fdb36c5c2aaaa1d612e7fb16fb9b03e64f7'
MANIFEST = ROOT / 'validation/am-pm-datapoints-order-captures-20261014.json'
MANIFEST_SHA256 = 'f798871df860c3df0e3a324f94742e416d41855a9423189d803098904010498f'
OBSERVATIONS = ROOT / 'validation/am-pm-datapoints-order-observations-20261014.json'
CAPTURE_SHA256 = '914b9ebcddca9865e604cd5651534f0aaa325065bbf947f762a598e44f3cda19'
INPUT_LABEL = '{1}D[(Source.Source1)],Source'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


m = module('allocation_normalizer_tests', SCRIPT)
synthetic = module('allocation_synthetic_output', SCRIPT.with_name('test_archive_am_pm_datapoints_zero_study.py'))


def node(capture, name):
    return next(item for item in capture['nodes'] if item['path'] == m.INPUT_PREFIX + name)


def synthetic_capture():
    """Synthetic test context with verbatim historical input nodes; not a run."""
    capture, profile = synthetic.raw_fixture()
    profile['name'] = capture['profile'] = 'synthetic-allocation-input'
    profile['source_phase_deg'] = 37
    synthetic.node(capture, m.SOURCE + 'Phase')['data'] = math.radians(37)
    synthetic.node(capture, m.PATH + '1/Eqns/VarBlock/DCPH')['data'][0] = math.radians(37)
    excerpt = json.loads(FIXTURE.read_bytes())
    capture['nodes'] = [item for item in capture['nodes'] if item['path'] not in m.INPUT_PATHS]
    capture['nodes'].extend(copy.deepcopy(excerpt['nodes']))
    # Keep the existing synthetic output spectrum bound to its desired label in
    # the verbatim historical mapping; unrelated zero power remains unrelated.
    synthetic.node(capture, m.RAW + '/Eqns/VarBlock/ID2')['data'] = [2334, 0, 2334]
    return capture, profile


def historical_input_sample():
    """Append independently transcribed input values to the old committed sample."""
    sample = copy.deepcopy(json.loads(OBSERVATIONS.read_bytes())['samples'][0])
    voltage = [0.025255072002712348, 0.019031061793281567]
    impedance = [49.999999742094296, 0]
    sample['ordinary'].update(
        raw_input_spectrum_nodes=copy.deepcopy(json.loads(FIXTURE.read_bytes())['nodes']),
        raw_input_w=9.999999800000004e-06,
        raw_input_carrier=[dict(raw_index=index, signal_id=2339, frequency_hz=frequency,
                                power_w=9.999999800000004e-06, voltage=voltage[:],
                                impedance=impedance[:], label=INPUT_LABEL)
                           for index, frequency in ((102, 99999999.5), (103, 100000000.5))])
    return sample


class AllocationNormalizerTests(unittest.TestCase):
    def setUp(self):
        self.capture, self.profile = synthetic_capture()

    def reject(self, mutation, message=None):
        capture = copy.deepcopy(self.capture)
        mutation(capture)
        with self.assertRaisesRegex(ValueError, message or '.'):
            m.normalize(capture, self.profile)

    def reject_sample(self, mutation, message=None):
        sample = historical_input_sample()
        mutation(sample)
        with self.assertRaisesRegex(ValueError, message or '.'):
            m.validate_raw_input(sample)

    def test_historical_excerpt_hash_and_committed_manifest_binding(self):
        self.assertEqual(hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), FIXTURE_SHA256)
        self.assertEqual(hashlib.sha256(MANIFEST.read_bytes()).hexdigest(), MANIFEST_SHA256)
        excerpt, manifest = json.loads(FIXTURE.read_bytes()), json.loads(MANIFEST.read_bytes())
        self.assertEqual(excerpt['classification'], 'retained_historical_input_spectrum_excerpt_not_new_measurement')
        self.assertEqual(excerpt['new_analysis_calls'], 0)
        self.assertIs(type(excerpt['new_analysis_calls']), int)
        self.assertEqual(excerpt['committed_order_manifest'], dict(
            path=MANIFEST.relative_to(ROOT).as_posix(), sha256=MANIFEST_SHA256, capture_index=0, profile='a-zero'))
        original = manifest['captures'][0]
        self.assertEqual((original['profile'], original['profile_index']), ('a-zero', 0))
        self.assertEqual(excerpt['source_capture']['path'], original['directory'] + '/capture.json')
        self.assertEqual(excerpt['source_capture']['sha256'], CAPTURE_SHA256)
        self.assertEqual(original['validation']['artifact_sha256']['capture.json'], CAPTURE_SHA256)
        self.assertEqual(len(excerpt['nodes']), 7)
        self.assertEqual({item['path'] for item in excerpt['nodes']}, m.INPUT_PATHS)

    def test_real_historical_input_is_physically_valid_and_dcp_difference_is_retained(self):
        sample = historical_input_sample()
        before = copy.deepcopy(sample)
        self.assertIsNone(m.validate_raw_input(sample))
        self.assertEqual(sample, before)
        self.assertEqual(sample['ordinary']['raw_input_w'], 9.999999800000004e-06)
        self.assertEqual(sample['ordinary']['input_power_w'], 9.999999750000005e-06)
        self.assertNotEqual(sample['ordinary']['raw_input_w'], sample['ordinary']['input_power_w'])

    def test_normalize_preserves_old_result_and_raw_values_types_order_and_metadata(self):
        original = copy.deepcopy((self.capture, self.profile))
        old = m.previous.normalize(self.capture, self.profile)
        result = m.normalize(self.capture, self.profile)
        self.assertEqual((self.capture, self.profile), original)
        ordinary = result['ordinary']
        excerpt_nodes = json.loads(FIXTURE.read_bytes())['nodes']
        self.assertEqual(ordinary['raw_input_spectrum_nodes'], excerpt_nodes)
        self.assertEqual(ordinary['raw_input_carrier'], historical_input_sample()['ordinary']['raw_input_carrier'])
        self.assertIs(type(ordinary['raw_input_carrier'][0]['signal_id']), int)
        self.assertIs(type(ordinary['raw_input_carrier'][0]['impedance'][1]), int)
        self.assertEqual(set(ordinary['raw_input_carrier'][0]),
                         {'raw_index', 'signal_id', 'frequency_hz', 'power_w', 'voltage', 'impedance', 'label'})
        for field in ('raw_input_spectrum_nodes', 'raw_input_carrier', 'raw_input_w'):
            ordinary.pop(field)
        self.assertEqual(result, old)

    def test_new_metadata_and_carrier_arrays_are_independent_copies(self):
        result = m.normalize(self.capture, self.profile)
        result['ordinary']['raw_input_spectrum_nodes'][0]['variables'].append('synthetic new metadata')
        result['ordinary']['raw_input_carrier'][0]['voltage'][0] = 0
        self.assertNotIn('synthetic new metadata', self.capture['nodes'][-7]['variables'])
        self.assertEqual(node(self.capture, 'V1')['data'][204], .025255072002712348)
        self.assertEqual(result['ordinary']['raw_input_spectrum_nodes'][4]['data'],
                         json.loads(FIXTURE.read_bytes())['nodes'][4]['data'])

    def test_generic_gain_source_allocation_keeps_si_guards(self):
        for gain in (0, 7, 20, 23):
            with self.subTest(gain=gain):
                capture, profile = synthetic_capture()
                profile['gain_db'] = gain
                source = 10 ** ((-gain - 30) / 10)
                synthetic.node(capture, m.AMP + 'G')['data'] = 10 ** (gain / 10)
                synthetic.node(capture, m.SOURCE + 'Pwr')['data'] = source
                synthetic.node(capture, m.PATH + '1/Eqns/VarBlock/DCP')['data'][0] = source
                factor = source / .00001
                for index in (102, 103):
                    node(capture, 'P1')['data'][index] *= factor
                    for offset in (2 * index, 2 * index + 1):
                        node(capture, 'V1')['data'][offset] *= math.sqrt(factor)
                result = m.normalize(capture, profile)
                self.assertEqual(result['parameters_si'][m.AMP + 'G'], 10 ** (gain / 10))
                self.assertEqual(result['ordinary']['input_power_w'], source)
                self.assertNotEqual(result['ordinary']['raw_input_w'], source)

    def test_frozen_acquisition_output_and_parameter_guards_still_run(self):
        self.reject(lambda c: c.update(manager_errors='synthetic warning'), 'Diagnostic run')
        self.reject(lambda c: c.update(analysis_done=0), 'Incomplete analysis')
        self.reject(lambda c: synthetic.node(c, m.AMP + 'G').update(data=101), 'Parameter unit/value mismatch')
        self.reject(lambda c: synthetic.node(c, m.RAW + '/Eqns/VarBlock/P2')['data'].__setitem__(0, .002),
                    'Raw V/Z/P inconsistency')

    def test_missing_duplicate_and_wrong_input_nodes_are_rejected(self):
        for name in m.INPUT_VARIABLES:
            with self.subTest(name=name, kind='missing'):
                self.reject(lambda c: c['nodes'].remove(node(c, name)))
            with self.subTest(name=name, kind='duplicate'):
                self.reject(lambda c: c['nodes'].append(copy.deepcopy(node(c, name))))
        self.reject(lambda c: node(c, 'F1').update(path='RFModel_AMPM_A01/PointDerivative/System1_Data/F1'))

    def test_input_nodes_with_evaluation_errors_are_rejected(self):
        for name in m.INPUT_VARIABLES:
            with self.subTest(name=name):
                self.reject(lambda c: node(c, name).update(evaluation_error='synthetic evaluation failure'))

    def test_all_input_vector_shapes_lengths_and_boolean_dimensions_are_checked(self):
        for name in m.INPUT_VARIABLES:
            for shape in (None, [], [1, 106], [106, 1], [True], [0]):
                with self.subTest(name=name, shape=shape):
                    self.reject(lambda c: node(c, name).update(dimensions=shape))
            with self.subTest(name=name, kind='scalar'):
                self.reject(lambda c: node(c, name).update(data=1))
            with self.subTest(name=name, kind='short'):
                self.reject(lambda c: node(c, name)['data'].pop())

    def test_mismatched_input_lengths_remain_invalid_with_consistent_local_shapes(self):
        for name in ('P1', 'ID1', 'V1', 'Z1'):
            def shorten(capture):
                item = node(capture, name)
                item['data'] = item['data'][:-1]
                item['dimensions'] = [len(item['data'])]
            with self.subTest(name=name):
                self.reject(shorten)

    def test_every_numeric_input_array_rejects_nonfinite_bool_and_text(self):
        for name in ('F1', 'P1', 'V1', 'Z1', 'ID1', 'IDNo'):
            for value in (math.nan, math.inf, -math.inf, True, '1', None, 10 ** 400):
                with self.subTest(name=name, value=str(value)[:20]):
                    self.reject(lambda c: node(c, name)['data'].__setitem__(0, value))

    def test_identifier_map_rejects_duplicates_fractional_and_unknown_ids(self):
        self.reject(lambda c: node(c, 'IDNo')['data'].__setitem__(0, 2339))
        self.reject(lambda c: node(c, 'IDNo')['data'].__setitem__(0, .5))
        self.reject(lambda c: node(c, 'ID1')['data'].__setitem__(0, .5))
        self.reject(lambda c: node(c, 'ID1')['data'].__setitem__(0, 999999))
        self.reject(lambda c: node(c, 'IDName')['data'].__setitem__(0, INPUT_LABEL))
        self.reject(lambda c: node(c, 'IDName')['data'].__setitem__(0, None))

    def test_label_cannot_select_output_node_total_noise_or_malformed_source(self):
        for label in ('{1}D[(Source.Source1)],Source,RFAmp', "Node Total from 'Source'",
                      "Node Noise from 'Source'", '{2}D[(Source.Source1)],Source',
                      '{1}D[],Source', '{1}D[(Source.Source1)],OtherSource',
                      '{1}D[(Source.Source1)],Source,', ' ' + INPUT_LABEL,
                      INPUT_LABEL + '\n'):
            with self.subTest(label=label):
                self.reject_sample(lambda s: node(dict(nodes=s['ordinary']['raw_input_spectrum_nodes']),
                                                  'IDName')['data'].__setitem__(11, label))

    def test_signal_id_values_and_full_spectrum_length_are_not_hardcoded(self):
        capture = copy.deepcopy(self.capture)
        node(capture, 'IDNo')['data'][11] = 4001
        for index in (102, 103):
            node(capture, 'ID1')['data'][index] = 4001
        # Append a non-carrier bin without changing the two desired inputs.
        for name, additions in (('F1', [0]), ('ID1', [0]), ('P1', [0]), ('V1', [0, 0]), ('Z1', [50, 0])):
            item = node(capture, name)
            item['data'].extend(additions)
            item['dimensions'] = [len(item['data'])]
        result = m.normalize(capture, self.profile)
        self.assertEqual([row['signal_id'] for row in result['ordinary']['raw_input_carrier']], [4001, 4001])
        self.assertEqual([row['raw_index'] for row in result['ordinary']['raw_input_carrier']], [102, 103])

    def test_two_desired_carriers_are_required_without_dropping_missing_or_extra(self):
        self.reject(lambda c: node(c, 'ID1')['data'].__setitem__(102, 0), 'exactly two')
        self.reject(lambda c: node(c, 'ID1')['data'].__setitem__(0, 2339), 'exactly two')

    def test_frequency_support_rejects_duplicate_side_wrong_center_and_negative_values(self):
        self.reject(lambda c: node(c, 'F1')['data'].__setitem__(102, 100000000.5), 'carrier support')
        self.reject(lambda c: node(c, 'F1')['data'].__setitem__(103, 100000001.5), 'carrier support')
        self.reject(lambda c: node(c, 'F1')['data'].__setitem__(0, -1), 'frequencies')
        self.reject_sample(lambda s: s['profile'].update(frequency_hz=915000000), 'carrier support')

    def test_original_input_carrier_order_is_preserved_when_both_bins_are_swapped(self):
        capture = copy.deepcopy(self.capture)
        node(capture, 'F1')['data'][102:104] = [100000000.5, 99999999.5]
        result = m.normalize(capture, self.profile)
        self.assertEqual([item['frequency_hz'] for item in result['ordinary']['raw_input_carrier']],
                         [100000000.5, 99999999.5])

    def test_raw_input_power_impedance_and_physical_consistency_are_checked(self):
        self.reject(lambda c: node(c, 'P1')['data'].__setitem__(0, -1), 'Negative raw input')
        self.reject(lambda c: node(c, 'P1')['data'].__setitem__(102, 0), 'power/impedance')
        self.reject(lambda c: node(c, 'P1')['data'].__setitem__(102, .00002), 'V/Z/P')
        for pair in ([0, 0], [-50, 0], [0, 50]):
            with self.subTest(impedance=pair):
                self.reject(lambda c: node(c, 'Z1')['data'].__setitem__(slice(204, 206), pair))
        self.reject(lambda c: node(c, 'V1')['data'].__setitem__(204, 1e308), 'V/Z/P')

    def test_input_phase_uses_circular_error_but_rejects_a_rotated_carrier(self):
        sample = historical_input_sample()
        sample['ordinary']['input_phase_rad'] += 2 * math.pi
        m.validate_raw_input(sample)

        def rotate(capture):
            values = node(capture, 'V1')['data']
            values[204], values[205] = -values[205], values[204]
        self.reject(rotate, 'input phase mismatch')

    def test_individually_physical_but_unequal_input_carrier_powers_are_rejected(self):
        def unequal(capture):
            node(capture, 'P1')['data'][103] *= 4
            node(capture, 'V1')['data'][206] *= 2
            node(capture, 'V1')['data'][207] *= 2
        self.reject(unequal, 'Nonconstant raw input')

    def test_public_validator_rejects_missing_new_fields_and_invalid_context(self):
        for name in ('raw_input_spectrum_nodes', 'raw_input_carrier', 'raw_input_w', 'input_phase_rad'):
            with self.subTest(name=name):
                self.reject_sample(lambda s: s['ordinary'].pop(name))
        self.reject_sample(lambda s: s['profile'].pop('frequency_hz'))
        self.reject_sample(lambda s: s.update(ordinary=[]))
        self.reject_sample(lambda s: s.update(profile=[]))
        self.reject_sample(lambda s: s['ordinary'].update(raw_input_spectrum_nodes=[dict(path=[])] * 7))

    def test_public_validator_requires_exact_carrier_fields_types_order_and_mean(self):
        for field, value in (('raw_index', 103), ('signal_id', 2339.0), ('label', INPUT_LABEL + ',RFAmp'),
                             ('frequency_hz', 100000000.5), ('power_w', math.nextafter(9.999999800000004e-6, math.inf))):
            with self.subTest(field=field):
                self.reject_sample(lambda s: s['ordinary']['raw_input_carrier'][0].update({field: value}), 'differ')
        self.reject_sample(lambda s: s['ordinary']['raw_input_carrier'][0]['impedance'].__setitem__(1, 0.0), 'differ')
        self.reject_sample(lambda s: s['ordinary']['raw_input_carrier'][0]['impedance'].__setitem__(1, -0.0), 'differ')
        self.reject_sample(lambda s: s['ordinary']['raw_input_carrier'].reverse(), 'differ')
        self.reject_sample(lambda s: s['ordinary']['raw_input_carrier'][0].update(derived_gain_db=20), 'differ')
        for value in (math.nextafter(9.999999800000004e-6, math.inf), math.nan, True):
            with self.subTest(raw_input_w=value):
                self.reject_sample(lambda s: s['ordinary'].update(raw_input_w=value), 'mean power')

    def test_cli_runs_offline_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source, plan, output = (directory / name for name in ('capture.json', 'plan.json', 'normalized.json'))
            source.write_text(json.dumps(self.capture), encoding='utf-8')
            plan.write_text(json.dumps(dict(profiles=[self.profile])), encoding='utf-8')
            command = [sys.executable, '-I', '-S', '-B', str(SCRIPT), str(source), str(plan), self.profile['name'], str(output)]
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(output.read_bytes()), m.normalize(self.capture, self.profile))
            original = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True, check=False).returncode, 0)
            self.assertEqual(output.read_bytes(), original)

    def test_cli_rejected_input_or_duplicate_profile_never_creates_output(self):
        for fault in ('raw-input', 'duplicate-profile'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                capture, profile = synthetic_capture()
                profiles = [profile]
                if fault == 'raw-input':
                    node(capture, 'F1')['data'][102] = 100000000
                else:
                    profiles.append(profile)
                source, plan, output = (directory / name for name in ('capture.json', 'plan.json', 'normalized.json'))
                source.write_text(json.dumps(capture), encoding='utf-8')
                plan.write_text(json.dumps(dict(profiles=profiles)), encoding='utf-8')
                completed = subprocess.run([sys.executable, '-I', '-S', '-B', str(SCRIPT), str(source), str(plan),
                                            profile['name'], str(output)], capture_output=True, check=False)
                self.assertNotEqual(completed.returncode, 0)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
