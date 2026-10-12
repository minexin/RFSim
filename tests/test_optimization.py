"""Native callback optimization regression, also runnable against an installed wheel."""
import json
import math
from pathlib import Path
import sys
import unittest

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
if '--installed' in sys.argv:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
import rfmodel
P = rfmodel.OptimizationParameter


class OptimizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = rfmodel.Library(LIBRARY_PATH)

    def test_mixed_and_replay(self):
        parameters = [P(-2, 2, .2), P(1e-9, 1e3, 1e-6, 'log'),
                      P(0, 10, 1, 'integer'), P(4, 4, 4)]
        seen = []
        def objective(x):
            self.assertIsInstance(x, tuple)
            seen.append(x)
            return (x[0]-.3)**2 + (math.log10(x[1])+3)**2 + (x[2]-7)**2
        result = self.lib.minimize_bounded(parameters, objective, minimum_step=1e-9, target=1e-15)
        self.assertEqual(result.stop, 'target_reached')
        self.assertLess(abs(result.parameters[0]-.3), 1e-7)
        self.assertLess(abs(result.parameters[1]/1e-3-1), 1e-7)
        self.assertEqual(result.parameters[2:], (7., 4.))
        self.assertEqual(tuple(seen), tuple(e.parameters for e in result.history))
        self.assertEqual(result.evaluations, len(seen))
        for entry in result.history:
            self.assertEqual(entry.status, 'finite')
            self.assertEqual(entry.parameters[2], int(entry.parameters[2]))
            for x, p in zip(entry.parameters, parameters):
                self.assertTrue(p.lower <= x <= p.upper)
        replay = self.lib.minimize_bounded(parameters, objective, minimum_step=1e-9, target=1e-15)
        self.assertEqual(result, replay)
        compact = self.lib.minimize_bounded(parameters, objective, minimum_step=1e-9,
                                            target=1e-15, record_history=False)
        self.assertEqual(compact, result._replace(history=()))
        json.dumps(result._asdict(), allow_nan=False)

    def test_stops_and_cancellation(self):
        minimize = self.lib.minimize_bounded
        p = [P(0, 1, .5)]
        self.assertEqual(minimize([P(2, 2, 2)], lambda x: x[0]).stop, 'all_parameters_fixed')
        self.assertEqual(minimize(p, lambda x: x[0], max_evaluations=1).stop, 'evaluation_limit')
        self.assertEqual(minimize(p, lambda x: x[0], max_iterations=1).stop, 'iteration_limit')
        self.assertEqual(minimize(p, lambda x: x[0]).stop, 'step_tolerance_reached')
        result = minimize(p, lambda x: self.fail('cancelled objective called'), cancelled=lambda: True)
        self.assertEqual(result.stop, 'cancelled')
        self.assertFalse(result.has_finite_point)
        self.assertIsNone(result.value)
        self.assertEqual(result.evaluations, 0)
        seen = []
        result = minimize(p, lambda x: seen.append(x) or x[0], cancelled=lambda: len(seen) == 2)
        self.assertEqual(result.stop, 'cancelled')
        self.assertEqual(result.evaluations, 2)
        self.assertTrue(result.has_finite_point)
        tiny = math.nextafter(1., math.inf)
        self.assertEqual(minimize([P(1., tiny, 1.)], lambda x: 0.).stop, 'numerical_resolution')

    def test_objective_failures(self):
        for invalid in (float('nan'), float('inf'), -float('inf')):
            result = self.lib.minimize_bounded([P(0, 1, .5)], lambda x: invalid)
            self.assertEqual(result.stop, 'objective_failure')
            self.assertEqual(result.history[0].status, 'nonfinite')
            self.assertIsNone(result.history[0].value)
            self.assertEqual(result.last_failed_parameters, (.5,))
            json.dumps(result._asdict(), allow_nan=False)
        def failed(x):
            raise ValueError('阻抗无效' * 300)
        result = self.lib.minimize_bounded([P(0, 1, .5)], failed)
        self.assertEqual(result.history[0].status, 'exception')
        self.assertTrue(result.last_error.startswith('ValueError: 阻抗'))
        self.assertLessEqual(len(result.last_error.encode('utf8')), 512)
        self.assertNotIn('\ufffd', result.last_error)
        result = self.lib.minimize_bounded([P(0, 1, .5)],
            lambda x: float('nan') if x[0] == .5 else (x[0]-.25)**2,
            invalid_evaluation='reject', target=0.)
        self.assertEqual(result.stop, 'target_reached')
        self.assertEqual(result.rejected_evaluations, 1)
        self.assertEqual(result.parameters, (.25,))
        result = self.lib.minimize_bounded([P(1, 1, 1)], failed, invalid_evaluation='reject')
        self.assertEqual(result.stop, 'no_finite_point')

    def test_fatal_exception_cleanup(self):
        for exception in (MemoryError, KeyboardInterrupt, SystemExit):
            seen = []
            def failed(x):
                seen.append(x)
                raise exception('test')
            with self.assertRaises(exception):
                self.lib.minimize_bounded([P(0, 1, .5)], failed, invalid_evaluation='reject')
            self.assertEqual(len(seen), 1)
        def cancel():
            raise RuntimeError('cancel hook failure')
        with self.assertRaisesRegex(RuntimeError, 'cancel hook failure'):
            self.lib.minimize_bounded([P(0, 1, .5)], lambda x: 0., cancelled=cancel)
        with self.assertRaisesRegex(TypeError, 'bool'):
            self.lib.minimize_bounded([P(0, 1, .5)], lambda x: 0., cancelled=lambda: 1)
        self.assertEqual(self.lib.minimize_bounded([P(1, 1, 1)], lambda x: 3.).value, 3.)

    def test_validation_before_callback(self):
        def objective(x):
            self.fail('invalid request called objective')
        for parameters in ([], [P(0, 1, 0)]*65, [P(0, 1, 2)], [P(0, 1, .5, 'integer')],
                           [P(0, 1, .5, 'log')], [P(0, 1, True)],
                           [dict(lower=0, upper=1, initial=0, surprise=1)]):
            with self.assertRaises((ValueError, RuntimeError)):
                self.lib.minimize_bounded(parameters, objective)
        for options in (dict(max_evaluations=True), dict(max_iterations=0), dict(target=float('nan')),
                        dict(contraction=1), dict(initial_step='0.1'), dict(record_history=1),
                        dict(invalid_evaluation='penalty')):
            with self.assertRaises((ValueError, RuntimeError)):
                self.lib.minimize_bounded([P(0, 1, .5)], objective, **options)

    def test_rf_matching_physical_reference(self):
        omega = 2*math.pi*1e9
        def objective(x):
            with self.lib.network() as network:
                network.add(self.lib.ideal_rlc(1e9, element='capacitor', connection='shunt', value=x[1]))
                network.add(self.lib.ideal_rlc(1e9, element='inductor', connection='series', value=x[0]))
                network.connect(1, 2)
                network.terminate(3, reflection=-1/3)
                return abs(network.external_s([0])[0][0])**2
        result = self.lib.minimize_bounded([P(1e-10, 3e-8, 2e-9, 'log'),
            P(1e-13, 1e-10, 1e-11, 'log')], objective, minimum_step=1e-9, target=1e-14)
        self.assertEqual(result.stop, 'target_reached')
        for value, reference in zip(result.parameters, (25/omega, .02/omega)):
            self.assertLess(abs(value/reference-1), 1e-6)
        for entry in result.history:
            inductance, capacitance = entry.parameters
            impedance = 1/(1/complex(25, omega*inductance)+complex(0, omega*capacitance))
            physical = abs((impedance-50)/(impedance+50))**2
            self.assertAlmostEqual(entry.value, physical, delta=1e-12)
        self.assertLessEqual(result.value, 1e-14)
        self.assertAlmostEqual(result.value, objective(result.parameters), delta=1e-22)


if __name__ == '__main__':
    unittest.main()
