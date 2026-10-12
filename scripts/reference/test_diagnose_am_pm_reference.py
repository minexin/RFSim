"""Keep native disagreement and quick-sweep source ownership visible."""
import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location('diagnostic', Path(__file__).with_name('diagnose-am-pm-reference.py'))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)
EVIDENCE = diagnostic.ROOT / 'validation/am-pm-existing-reference-evidence-20261012.json'


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.evidence = diagnostic.archive.load(EVIDENCE)
        self.library = diagnostic.Library(LIBRARY)

    def analyze(self):
        return diagnostic.analyze(self.evidence, self.library)

    def test_real_native_failure_not_replaced_by_fit(self):
        result = self.analyze()
        native = result['point_derivative_native']
        self.assertEqual((native['passed_count'], native['point_count']), (7, 37))
        self.assertFalse(native['passed'])
        self.assertAlmostEqual(native['max_circular_error_deg'], .00616264490543, delta=1e-10)
        fit = result['exploratory_cubic_fit']
        self.assertLess(fit['remaining_max_circular_error_deg'], 1e-8)
        self.assertFalse(fit['changes_native_verdict'])
        self.assertFalse(result['eligible_for_full_model_compatibility'])

    def test_quick_sweep_zero_dbm_counterexamples_preserved(self):
        result = self.analyze()['quick_sweep_natural_cubic_candidate']
        self.assertEqual((result['passed_count'], result['point_count']), (144, 148))
        failed = [r for r in result['rows'] if not r['passed']]
        self.assertEqual(len(failed), 4)
        self.assertTrue(all(r['index'] == 0 and r['output_dbm'] == 0 for r in failed))
        self.assertFalse(result['passed'])

    def test_normal_source_is_not_sweep_source(self):
        rows = self.analyze()['quick_sweep_natural_cubic_candidate']['rows']
        rows = [r for r in rows if r['profile'] == 'source-phase-offset']
        self.assertTrue(all(abs(r['normal_source_phase_deg'] - 30) < 1e-12 for r in rows))
        self.assertTrue(all(r['sweep_source_phase_deg'] == 0 for r in rows))
        self.assertAlmostEqual(rows[1]['candidate_phase_deg'], 20)
        scan = self.evidence['quick_sweeps'][2]
        scan['points'][1]['input_phase_rad'] = math.radians(7)
        scan['sweep_source_phase_deg'][1] = 7
        changed = self.analyze()['quick_sweep_natural_cubic_candidate']['rows']
        row = next(r for r in changed if r['profile'] == 'source-phase-offset' and r['index'] == 1)
        self.assertAlmostEqual(row['candidate_phase_deg'], 27)
        self.assertFalse(row['passed'])

    def test_invalid_or_incomplete_evidence_rejected(self):
        original = copy.deepcopy(self.evidence)
        mutations = [
            lambda d: d.update(phase_absolute_tolerance_degrees=.01),
            lambda d: d['point_derivative']['parameters_si'].update(PwrAMtoPM=10),
            lambda d: d['point_derivative'].update(dataset_timestamp='0'),
            lambda d: d['point_derivative']['points'].pop(),
            lambda d: d['point_derivative']['points'][0].update(output_power_w=-1),
            lambda d: d['point_derivative']['points'][0].update(phase_rad=float('nan')),
            lambda d: d['quick_sweeps'][0]['points'].pop(),
            lambda d: d['quick_sweeps'][0]['points'][0].update(input_phase_rad=1),
            lambda d: d['quick_sweeps'][0].update(dataset_path='ordinary-spectrum'),
            lambda d: d['quick_sweeps'].__setitem__(1, copy.deepcopy(d['quick_sweeps'][0])),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutations.index(mutation)):
                self.evidence = copy.deepcopy(original)
                mutation(self.evidence)
                with self.assertRaises(ValueError):
                    self.analyze()

    def test_cli_reports_failure_and_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'result.json'
            command = [sys.executable, str(Path(diagnostic.__file__)), str(EVIDENCE), str(LIBRARY), str(output)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 1, first.stderr)
            saved = output.read_bytes()
            result = json.loads(saved)
            self.assertFalse(result['point_derivative_native']['passed'])
            self.assertEqual(result['native_library_sha256'], diagnostic.archive.digest(LIBRARY))
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(second.returncode, 2)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == '__main__':
    unittest.main()
