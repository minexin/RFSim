"""Retain transfer-test verdicts and reject corrupted acquisition evidence."""
import copy
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

if len(sys.argv) < 3 or sys.argv[1] != '--native-probe':
    raise SystemExit('Usage: test_diagnose_am_pm_parameter_study.py --native-probe PROBE')
PROBE = Path(sys.argv.pop(2)).resolve()
sys.argv.pop(1)
spec = importlib.util.spec_from_file_location('diagnostic', Path(__file__).with_name('diagnose-am-pm-parameter-study.py'))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)
ROOT = Path(__file__).resolve().parents[2]
PLAN = ROOT / 'validation/am-pm-point-derivative-plan-20261013.json'
EVIDENCE = ROOT / 'validation/am-pm-parameter-study-observations-20261013.json'


def raw_fixture(sample):
    """Reconstruct only the required raw fields from the committed observation."""
    study = diagnostic.study
    nodes = []
    def add(path, data=None, timestamp=None):
        nodes.append(dict(path=path, data=data, dimensions=[len(data)] if isinstance(data, list) else None,
                          timestamp=timestamp))
    for path, data in sample['parameters_si'].items():
        add(path, data)
    for path, stamp in sample['timestamps'].items():
        add(path, timestamp=stamp)
    for suffix, points in [('1', [sample['ordinary']]), ('2', sample['sweep'])]:
        key = study.PATH + suffix + '/Eqns/VarBlock/'
        add(key+'NodeNames', ['1', '2']*len(points))
        add(key+'CF', [1e8]*2*len(points))
        add(key+'DCP', [p[k] for p in points for k in ('input_power_w', 'output_power_w')])
        add(key+'DCPH', [p[k] for p in points for k in ('input_phase_rad', 'output_phase_rad')])
    key = study.RAW + '/Eqns/VarBlock/'
    carriers = sample['ordinary']['raw_carrier']
    add(key+'IDNo', [1])
    add(key+'IDName', [carriers[0]['label']])
    add(key+'ID2', [1, 1])
    add(key+'F2', [c['frequency_hz'] for c in carriers])
    add(key+'P2', [c['power_w'] for c in carriers])
    add(key+'V2', [v for c in carriers for v in c['voltage']])
    add(key+'Z2', [v for c in carriers for v in c['impedance']])
    return dict(profile=sample['profile']['name'], analysis_done=1, manager_errors='', nodes=nodes,
                run_started_utc=sample['run_started_utc'], run_returned_utc=sample['run_returned_utc'])


class NoNativeCalls:
    def am_pm_response(self, *args, **kwargs):
        raise AssertionError('Corrupted evidence reached the native calculation')


class ParameterStudyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = diagnostic.archive.load(PLAN)
        cls.evidence = diagnostic.archive.load(EVIDENCE)
        cls.result = diagnostic.analyze(cls.evidence, cls.plan, diagnostic.NativeReplay(PROBE))

    def test_frozen_transfer_does_not_replace_native_verdict(self):
        summary = self.result['axes']['sweep']['summary']
        self.assertEqual(summary['native_analytic']['passed_count'], 73)
        self.assertEqual(summary['rounded_shift_only']['passed_count'], 131)
        self.assertAlmostEqual(summary['native_analytic']['max_circular_error_deg'], .0086277034447, delta=1e-10)
        for name in ('rounded_shift_frozen_scale', 'frozen_retrospective_mapping'):
            self.assertEqual(summary[name]['passed_count'], 438)
            self.assertLess(summary[name]['max_circular_error_deg'], 3e-7)
            for axis in ('ordinary_report', 'ordinary_raw'):
                self.assertEqual(self.result['axes'][axis]['summary'][name]['passed_count'], 6)
        self.assertEqual(self.result['parameters_fitted_this_study'], 0)
        self.assertFalse(self.result['eligible_for_full_model_compatibility'])
        self.assertFalse(self.result['product_formula_changed'])

    def test_all_native_sweep_passes_are_zero_phase_controls(self):
        rows = self.result['axes']['sweep']['rows']
        passed = [r for r in rows if r['hypotheses']['native_analytic']['passed']]
        self.assertEqual({r['profile'] for r in passed}, {'zero-phase'})
        self.assertEqual(len(passed), 73)
        self.assertTrue(all(r['input_phase_rad'] == 0 for r in rows))
        self.assertTrue(all(abs(r['input_phase_rad'] - diagnostic.math.pi/6) < 1e-10
                            for r in self.result['axes']['ordinary_report']['rows']))

    def test_normalization_roundtrip(self):
        for sample, profile in zip(self.evidence['samples'], self.plan['profiles']):
            actual = diagnostic.study.normalize(raw_fixture(sample), profile)
            expected = {k: v for k, v in sample.items() if k != 'capture_source'}
            self.assertEqual(actual, expected)

    def test_capture_guards(self):
        original = raw_fixture(self.evidence['samples'][0])
        profile = self.plan['profiles'][0]
        def modify_node(capture, suffix, field, value):
            next(n for n in capture['nodes'] if n['path'].endswith(suffix))[field] = value
        mutations = [
            lambda r: r.update(manager_errors='warning'),
            lambda r: r.update(analysis_done=0),
            lambda r: r.update(profile='different'),
            lambda r: modify_node(r, '/System1_Data', 'timestamp', '0'),
            lambda r: modify_node(r, '/StepPwr', 'data', .25),
            lambda r: modify_node(r, '/SweepType', 'data', 0),
            lambda r: modify_node(r, '/AutoLimits', 'data', 1),
            lambda r: modify_node(r, '/PwrAMtoPM', 'data', 10),
            lambda r: modify_node(r, '/DCP', 'dimensions', [1, 2]),
            lambda r: modify_node(r, '/IDName', 'data', ['unrelated']),
            lambda r: modify_node(r, '/P2', 'data', [1, 1]),
            lambda r: r['nodes'].append(copy.deepcopy(r['nodes'][0])),
        ]
        for i, mutation in enumerate(mutations):
            with self.subTest(mutation=i):
                raw = copy.deepcopy(original)
                mutation(raw)
                with self.assertRaises(ValueError):
                    diagnostic.study.normalize(raw, profile)

    def test_scorer_guards_before_native_execution(self):
        mutations = [
            lambda e: e.update(phase_absolute_tolerance_degrees=.01),
            lambda e: e['hypotheses'][1].update(reference_shift_db=0),
            lambda e: e['samples'].pop(),
            lambda e: e['samples'][0]['profile'].update(reference_dbm=11),
            lambda e: e['samples'][0]['timestamps'].update({diagnostic.study.RAW: 0}),
            lambda e: e['samples'][0]['parameters_si'].update({diagnostic.study.AMP+'G': 1}),
            lambda e: e['samples'][0]['sweep'].pop(),
            lambda e: e['samples'][0]['sweep'][0].update(index=1),
            lambda e: e['samples'][0]['sweep'][0].update(nominal_output_dbm=.1),
            lambda e: e['samples'][0]['sweep'][0].update(input_power_w=.1),
            lambda e: e['samples'][0]['sweep'][0].update(output_power_w=float('nan')),
            lambda e: e['samples'][0]['sweep'][0].update(input_phase_rad=diagnostic.math.pi/6),
        ]
        for i, mutation in enumerate(mutations):
            with self.subTest(mutation=i):
                evidence = copy.deepcopy(self.evidence)
                mutation(evidence)
                with self.assertRaises(ValueError):
                    diagnostic.analyze(evidence, self.plan, NoNativeCalls())

    def test_cli_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'result.json'
            output.write_bytes(b'preserve')
            run = subprocess.run([sys.executable, '-I', '-S', diagnostic.__file__, str(PLAN), str(EVIDENCE),
                                  str(output), '--native-probe', str(PROBE)], capture_output=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(output.read_bytes(), b'preserve')


if __name__ == '__main__':
    unittest.main()
