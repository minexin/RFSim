"""Direct C++ adapter replay and strict retention of holdout caveats."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[1] != '--native-probe':
    raise SystemExit('Usage: test_systemvue_am_pm_adapter.py --native-probe PROBE')
PROBE = Path(sys.argv.pop(2)).resolve()
sys.argv.pop(1)
spec = importlib.util.spec_from_file_location('diagnostic', Path(__file__).with_name('diagnose-systemvue-am-pm-adapter.py'))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class NoCalls:
    def am_pm_response(self, *args, **kwargs):
        raise AssertionError('Invalid evidence reached the native API')
    systemvue2023_am_pm_response = am_pm_response


def capture_fixture(sample):
    """Reconstruct the raw COM field schema, including the unrequested endpoint."""
    study, nodes = diagnostic.study, []
    def add(path, value=None, timestamp=None):
        nodes.append(dict(path=path, data=value, timestamp=timestamp,
                          dimensions=[len(value)] if isinstance(value, list) else None))
    for key, value in sample['parameters_si'].items():
        add(key, value)
    for key, value in sample['timestamps'].items():
        add(key, timestamp=value)
    for suffix, points in [('1', [sample['ordinary']]),
                           ('2', sample['sweep']+sample['additional_sweep_points'])]:
        key = study.PATH+suffix+'/Eqns/VarBlock/'
        add(key+'NodeNames', ['1', '2']*len(points))
        add(key+'CF', [sample['profile']['frequency_hz']]*2*len(points))
        for field, names in [('DCP', ('input_power_w', 'output_power_w')),
                             ('DCPH', ('input_phase_rad', 'output_phase_rad'))]:
            add(key+field, [point[name] for point in points for name in names])
    carrier, key = sample['ordinary']['raw_carrier'], study.RAW+'/Eqns/VarBlock/'
    add(key+'IDNo', [1])
    add(key+'IDName', [carrier[0]['label']])
    add(key+'ID2', [1, 1])
    for field, name in [('F2', 'frequency_hz'), ('P2', 'power_w')]:
        add(key+field, [c[name] for c in carrier])
    for field, name in [('V2', 'voltage'), ('Z2', 'impedance')]:
        add(key+field, [v for c in carrier for v in c[name]])
    return dict(profile=sample['profile']['name'], analysis_done=1, nodes=nodes,
                manager_errors=sample['diagnostics'], run_started_utc=sample['run_started_utc'],
                run_returned_utc=sample['run_returned_utc'])


class AdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old = diagnostic.archive.load(diagnostic.ROOT/'validation/am-pm-parameter-study-observations-20261013.json')
        cls.new = diagnostic.archive.load(diagnostic.ROOT/'validation/am-pm-adapter-observations-20261013.json')
        cls.plan = diagnostic.archive.load(diagnostic.OLD_PLAN)
        cls.amendment = diagnostic.archive.load(diagnostic.AMENDMENT)
        cls.native = diagnostic.NativeReplay(PROBE)
        cls.result = diagnostic.analyze(cls.old, cls.new, cls.plan, cls.amendment, cls.native)

    def test_direct_calibrated_cpp_response(self):
        result = self.result['axes']['registered_sweep']
        calibrated = result['all_points']['systemvue2023_calibrated']
        self.assertEqual((calibrated['point_count'], calibrated['numeric_passed_count']), (586, 586))
        self.assertLess(calibrated['max_circular_error_deg'], 3e-7)
        self.assertEqual(result['warning_free']['systemvue2023_calibrated']['point_count'], 549)
        self.assertLess(result['all_points']['legacy_analytic']['numeric_passed_count'], 586)
        self.assertEqual({e['operation'] for e in self.native.exchanges}, {'am_pm', 'systemvue2023_am_pm'})
        self.assertEqual(sum(e['operation'] == 'systemvue2023_am_pm' for e in self.native.exchanges), 610)

    def test_extra_points_and_warning_are_not_discarded(self):
        self.assertFalse(self.result['eligible_for_full_model_compatibility'])
        axes = self.result['axes']
        self.assertEqual(axes['registered_sweep']['warning_diagnostic']['systemvue2023_calibrated']['point_count'], 37)
        self.assertEqual(axes['additional_sweep']['all_points']['systemvue2023_calibrated']['point_count'], 4)
        self.assertEqual(axes['additional_sweep']['warning_diagnostic']['systemvue2023_calibrated']['point_count'], 1)
        for axis in ('ordinary_report', 'ordinary_raw'):
            self.assertEqual(axes[axis]['all_points']['systemvue2023_calibrated']['numeric_passed_count'], 10)
            self.assertEqual(axes[axis]['warning_free']['systemvue2023_calibrated']['point_count'], 9)

    def test_frozen_byte_chain(self):
        self.assertEqual(diagnostic.archive.digest(diagnostic.PLAN), diagnostic.current.PLAN_SHA)
        self.assertEqual(diagnostic.archive.digest(diagnostic.AMENDMENT), self.new['amendment_sha256'])
        manifest = diagnostic.ROOT/'validation/am-pm-adapter-captures-20261013.json'
        self.assertEqual(diagnostic.archive.digest(manifest), self.new['manifest_sha256'])

    def test_capture_normalization_preserves_warning_and_extra_grid(self):
        for sample, profile in zip(self.new['samples'], self.amendment['effective_profiles']):
            capture = capture_fixture(sample)
            actual = diagnostic.study.normalize(capture, profile, allow_diagnostic=True,
                                                retain_extra_endpoint=True)
            expected = {k: v for k, v in sample.items() if k not in ('completion_basis', 'capture_source')}
            self.assertEqual(actual, expected)
            with self.assertRaises(ValueError):
                diagnostic.study.normalize(capture, profile)
        flagged = self.new['samples'][2]
        with self.assertRaises(ValueError):
            diagnostic.study.normalize(capture_fixture(flagged), flagged['profile'], retain_extra_endpoint=True)

    def test_capture_wrong_frequency_and_voltage_rejected(self):
        sample = self.new['samples'][0]
        for suffix in ('/CF', '/F2', '/V2'):
            with self.subTest(field=suffix):
                capture = capture_fixture(sample)
                node = next(n for n in capture['nodes'] if n['path'].endswith(suffix))
                node['data'][0] = 0
                with self.assertRaises(ValueError):
                    diagnostic.study.normalize(capture, sample['profile'], allow_diagnostic=True,
                                               retain_extra_endpoint=True)

    def test_invalid_evidence_rejected_before_native_call(self):
        mutations = [
            lambda e: e.update(phase_absolute_tolerance_degrees=.001),
            lambda e: e['samples'].pop(),
            lambda e: e['samples'][0]['sweep'].pop(),
            lambda e: e['samples'][0]['additional_sweep_points'].clear(),
            lambda e: e['samples'][0]['sweep'][0].update(index=1),
            lambda e: e['samples'][0]['profile'].update(frequency_hz=1e8),
            lambda e: e['samples'][0]['parameters_si'].update({diagnostic.study.AMP+'G': 100}),
            lambda e: e['samples'][0]['timestamps'].update({diagnostic.study.RAW: 0}),
            lambda e: e['samples'][0]['ordinary'].update(input_phase_rad=0),
            lambda e: e['samples'][2].update(diagnostics='', warning_free_acceptance_eligible=True),
            lambda e: e['samples'][2].update(warning_free_acceptance_eligible=True),
        ]
        for i, mutation in enumerate(mutations):
            with self.subTest(mutation=i):
                new = copy.deepcopy(self.new)
                mutation(new)
                with self.assertRaises(ValueError):
                    diagnostic.analyze(self.old, new, self.plan, self.amendment, NoCalls())


if __name__ == '__main__':
    unittest.main()
