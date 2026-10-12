"""Monte Carlo callback/snapshot regression against source or an installed wheel."""
import itertools
import json
import math
from pathlib import Path
import statistics
import sys
import unittest
from unittest.mock import patch

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
if '--installed' in sys.argv:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
import rfmodel
P, L = rfmodel.MonteCarloParameter, rfmodel.MonteCarloLimits


class MonteCarloTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = rfmodel.Library(LIBRARY_PATH)

    def run_mc(self, parameters=None, metric_count=1, evaluate=lambda x: x, **options):
        return self.lib.run_monte_carlo(parameters or [P('fixed', 1)], metric_count, evaluate, **options)

    def test_golden_random_stream(self):
        r = self.run_mc([P('uniform', 0, 1)], samples=10000, seed=5489)
        self.assertEqual(r.history[-1].parameters[0], (9981545732273789042 >> 11)*2**-53)
        self.assertEqual((r.random_words, r.accepted_samples, r.evaluations), (10000,)*3)
        self.assertEqual(r.algorithm, 'mt19937_64-box-muller52-v1')

    def test_six_distributions_replay(self):
        p = [P('fixed', 7), P('uniform', -3, 4), P('log_uniform', .1, 10),
             P('normal', 2, .3), P('log_normal', 0, .5), P('discrete_uniform', -3, 7)]
        r = self.run_mc(p, 6, samples=256, seed=2**64-1)
        self.assertEqual(r, self.run_mc(p, 6, samples=256, seed=2**64-1))
        self.assertEqual(r._replace(history=()), self.run_mc(p, 6, samples=256, seed=2**64-1, record_history=False))
        self.assertNotEqual(r.statistics[1].mean, self.run_mc(p, 6, samples=256, seed=0).statistics[1].mean)
        for e in r.history:
            self.assertEqual(e.parameters, e.values)
            self.assertEqual(e.parameters[0], 7)
            self.assertTrue(-3 <= e.parameters[1] <= 4 and .1 <= e.parameters[2] <= 10)
            self.assertEqual(e.parameters[5], int(e.parameters[5]))
            self.assertIsNone(e.passed)
        json.dumps(r._asdict(), allow_nan=False)
        with self.assertRaises(AttributeError):
            r.seed = 1

    def test_degenerate_and_defaults(self):
        p = [P('fixed', 2), P('uniform', 2, 2), P('log_uniform', 2, 2),
             P('normal', 2, 0), P('log_normal', 0, 0), P('discrete_uniform', 2, 2)]
        r = self.run_mc(p, 6)
        self.assertEqual((r.seed, r.requested_samples, r.random_words), (0, 1000, 0))
        self.assertEqual(r.quantile_levels, (.05, .5, .95))
        self.assertEqual(tuple(s.mean for s in r.statistics), (2, 2, 2, 2, 1, 2))
        self.assertTrue(all(s.sample_variance == 0 for s in r.statistics))
        self.assertFalse(r.criteria_enabled)
        self.assertIsNone(r.passed_fraction)

    def test_exact_statistics(self):
        values = iter([1, 2, 3, 4])
        r = self.run_mc(evaluate=lambda _: [next(values)], samples=4, quantile_levels=[0, .25, .5, .75, 1])
        s = r.statistics[0]
        self.assertEqual((s.count, s.mean, s.minimum, s.maximum), (4, 2.5, 1, 4))
        self.assertAlmostEqual(s.sample_variance, 5/3)
        self.assertAlmostEqual(s.standard_error, math.sqrt(5/12))
        self.assertEqual(s.quantiles, (1, 1.75, 2.5, 3.25, 4))
        s = self.run_mc(samples=1).statistics[0]
        self.assertIsNone(s.sample_variance)
        self.assertIsNone(s.sample_standard_deviation)
        self.assertIsNone(s.standard_error)
        self.assertEqual(self.run_mc(samples=2, quantile_levels=[]).statistics[0].quantiles, ())

    def test_extreme_statistics(self):
        a = 1e150
        b = math.nextafter(a, math.inf)
        values = iter([a, b])
        s = self.run_mc(evaluate=lambda _: [next(values)], samples=2).statistics[0]
        self.assertAlmostEqual(s.sample_variance/((b-a)**2/2), 1, delta=1e-14)
        maximum = sys.float_info.max
        values = iter([-maximum, maximum])
        r = self.run_mc(evaluate=lambda _: [next(values)], samples=2)
        s = r.statistics[0]
        self.assertEqual(s.mean, 0)
        self.assertIsNone(s.sample_variance)
        self.assertIsNone(s.sample_standard_deviation)
        self.assertEqual(s.standard_error, maximum)
        json.dumps(r._asdict(), allow_nan=False)

    def test_measurement_failures(self):
        for value, status in [([], 'invalid_measurement_count'), ([1, 2], 'invalid_measurement_count'),
                              ([math.nan], 'nonfinite_measurement'), ([math.inf], 'nonfinite_measurement'),
                              ([True], 'exception'), (None, 'exception')]:
            r = self.run_mc(evaluate=lambda _: value, samples=4)
            self.assertEqual(r.stop, 'evaluation_failure')
            self.assertEqual((r.samples_attempted, r.evaluations, r.rejected_samples), (1, 1, 1))
            self.assertEqual(r.history[0].status, status)
            self.assertEqual(r.history[0].values, ())
            self.assertFalse(r.has_statistics)
            self.assertIsNone(r.statistics[0].mean)
            json.dumps(r._asdict(), allow_nan=False)

    def test_exception_diagnostics(self):
        def invalid(_):
            raise ValueError('电阻无效' * 300)
        r = self.run_mc(evaluate=invalid)
        self.assertTrue(r.last_error.startswith('ValueError: 电阻'))
        self.assertLessEqual(len(r.last_error.encode('utf-8')), 512)
        self.assertNotIn('\ufffd', r.last_error)
        self.assertEqual(r.history[0].error, r.last_error)

    def test_failure_denominators_and_attempt_budget(self):
        for policy, expected in [('stop', (2, 1, 1, .5, 1)), ('reject', (4, 3, 2, .5, 2/3))]:
            seen = []
            def evaluate(x):
                seen.append(x)
                if len(seen) == 2:
                    raise ValueError('invalid circuit')
                return [len(seen)-1]
            r = self.run_mc([P('uniform', 0, 1)], evaluate=evaluate, samples=4,
                            invalid_sample=policy, criteria=[L(0, 2)])
            self.assertEqual((r.samples_attempted, r.accepted_samples, r.passed_samples,
                              r.passed_fraction, r.valid_pass_fraction), expected)
            self.assertEqual(r.random_words, len(seen))
            self.assertEqual(r.evaluations, len(seen))
        r = self.run_mc(evaluate=lambda _: [], samples=4, invalid_sample='reject', criteria=[L(upper=2)])
        self.assertEqual((r.stop, r.samples_attempted, r.accepted_samples, r.passed_fraction), ('completed', 4, 0, 0))
        self.assertIsNone(r.valid_pass_fraction)
        json.dumps(r._asdict(), allow_nan=False)

    def test_sampling_failures_and_json(self):
        for first, expected in [(10000, None), (-10000, 0.)]:
            r = self.run_mc([P('log_normal', first, .01), P('uniform', 0, 1)],
                            evaluate=lambda _: self.fail('unrepresentable point reached evaluator'),
                            samples=3, invalid_sample='reject')
            self.assertEqual((r.samples_attempted, r.evaluations, r.rejected_samples, r.random_words), (3, 0, 3, 9))
            self.assertEqual(r.history[0].parameters[0], expected)
            self.assertEqual(r.last_failed_parameters[0], expected)
            self.assertEqual(r.history[0].status, 'sampling_failure')
            json.dumps(r._asdict(), allow_nan=False)

    def test_cancellation(self):
        r = self.run_mc([P('uniform', 0, 1)], cancelled=lambda: True)
        self.assertEqual((r.stop, r.random_words, r.evaluations, r.samples_attempted), ('cancelled', 0, 0, 0))
        self.assertIsNone(r.statistics[0].mean)
        seen = []
        r = self.run_mc([P('uniform', 0, 1)], evaluate=lambda x: seen.append(x) or x,
                        cancelled=lambda: len(seen) == 2)
        self.assertEqual((r.stop, r.samples_attempted, r.random_words), ('cancelled', 2, 2))

    def test_fatal_exceptions_and_cleanup(self):
        real_destroy = self.lib._dll.rfmodel_monte_carlo_destroy
        destroyed = []
        def destroy(handle):
            destroyed.append(handle.value)
            real_destroy(handle)
        with patch.object(self.lib._dll, 'rfmodel_monte_carlo_destroy', destroy):
            for exception in (MemoryError, KeyboardInterrupt, SystemExit):
                seen = []
                def evaluate(x):
                    seen.append(x)
                    raise exception('test')
                before = len(destroyed)
                with self.assertRaises(exception):
                    self.run_mc(evaluate=evaluate, invalid_sample='reject')
                self.assertEqual(len(seen), 1)
                self.assertEqual(len(destroyed), before+1)
            with patch.object(self.lib._dll, 'rfmodel_monte_carlo_get_statistics', side_effect=RuntimeError('read failed')):
                before = len(destroyed)
                with self.assertRaisesRegex(RuntimeError, 'read failed'):
                    self.run_mc(samples=1)
                self.assertEqual(len(destroyed), before+1)
        self.assertEqual(self.run_mc(samples=1).accepted_samples, 1)

    def test_cancel_and_exception_formatting_errors(self):
        def cancel():
            raise RuntimeError('cancel failed')
        with self.assertRaisesRegex(RuntimeError, 'cancel failed'):
            self.run_mc(cancelled=cancel)
        with self.assertRaisesRegex(TypeError, 'bool'):
            self.run_mc(cancelled=lambda: 1)
        class BadMessage(Exception):
            def __str__(self):
                raise RuntimeError('formatting failed')
        def fail(_):
            raise BadMessage()
        with self.assertRaisesRegex(RuntimeError, 'formatting failed'):
            self.run_mc(evaluate=fail, invalid_sample='reject')

    def test_input_validation_before_callbacks(self):
        seen = []
        def evaluate(x):
            seen.append(x)
            return x
        parameters = [[], [P('fixed', 1)]*65, [P('fixed', True)], [P('uniform', 2, 1)],
            [P('log_uniform', 0, 1)], [P('normal', 0, -1)], [P('discrete_uniform', .5, 2)],
            [P('log_normal', 10000, 0)], [P('other', 0)], [dict(distribution='fixed', first=1, surprise=2)]]
        for p in parameters:
            with self.assertRaises((ValueError, RuntimeError)):
                self.lib.run_monte_carlo(p, 1, evaluate)
        for options in [dict(samples=True), dict(samples=0), dict(samples=100001), dict(seed=-1),
            dict(seed=2**64), dict(seed=1.), dict(seed=True), dict(record_history=1), dict(invalid_sample='resample'),
            dict(quantile_levels=[.5, .5]), dict(quantile_levels=[math.nan]), dict(quantile_levels=[2]),
            dict(criteria=[L(2, 1)]), dict(criteria=[L()]), dict(criteria=[L(upper=1), L()]),
            dict(criteria=[dict(other=1)]), dict(criteria=[L(upper=math.inf)])]:
            with self.assertRaises((ValueError, RuntimeError)):
                self.lib.run_monte_carlo([P('fixed', 1)], 1, evaluate, **options)
        for m in [True, 0, 65, 1.]:
            with self.assertRaises(ValueError):
                self.lib.run_monte_carlo([P('fixed', 1)], m, evaluate)
        self.assertEqual(seen, [])

    def test_frozen_request_and_callback_reentry(self):
        p = [dict(distribution='fixed', first=1)]
        q = [0., .5, 1.]
        limits = [dict(upper=1)]
        options = dict(samples=3, quantile_levels=q, criteria=limits)
        def evaluate(x):
            self.assertIsInstance(x, tuple)
            with self.lib.network():
                pass
            p[0]['first'] = 100
            q[:] = [.9]
            limits[0]['upper'] = -1
            options['samples'] = 1
            return x
        r = self.lib.run_monte_carlo(p, 1, evaluate, **options)
        self.assertEqual((r.accepted_samples, r.passed_samples, r.quantile_levels), (3, 3, (0., .5, 1.)))
        self.assertEqual(r.statistics[0].mean, 1)

    def test_inclusive_multimetric_criteria(self):
        r = self.run_mc([P('fixed', 1), P('fixed', 2)], 2, samples=2,
                        criteria=[L(1, 1), L(upper=2)])
        self.assertEqual(r.passed_samples, 2)
        r = self.run_mc([P('fixed', 1), P('fixed', 2)], 2, samples=2,
                        criteria=[L(1, 1), L(upper=1)])
        self.assertEqual(r.passed_samples, 0)

    def test_bounded_iterables(self):
        with self.assertRaises(ValueError):
            self.lib.run_monte_carlo(itertools.repeat(P('fixed', 1)), 1, lambda x: x)
        with self.assertRaises(ValueError):
            self.run_mc(quantile_levels=itertools.repeat(.5))
        r = self.run_mc(evaluate=lambda _: itertools.repeat(1), samples=1)
        self.assertEqual(r.history[0].status, 'invalid_measurement_count')
        self.assertEqual(self.run_mc(evaluate=lambda x: (v for v in x), samples=1).accepted_samples, 1)

    def test_rf_resistor_tolerance(self):
        def evaluate(x):
            with self.lib.network() as network:
                for resistance in x:
                    network.add(self.lib.ideal_rlc(1e9, element='resistor', connection='series', value=resistance))
                network.connect(1, 2)
                s = network.external_s([0, 3])
            return [abs(s[1][0])**2, abs(s[0][0])**2]
        r = self.run_mc([P('uniform', 45, 55), P('fixed', 50)], 2, evaluate,
                        samples=4096, seed=20261012, criteria=[L(lower=.25), L(upper=.25)])
        self.assertEqual((r.accepted_samples, r.passed_samples), (4096, 2060))
        self.assertEqual(r.passed_fraction, 2060/4096)
        cols = [[], []]
        for e in r.history:
            a, b = e.parameters
            expected = [(100/(100+a+b))**2, ((a+b)/(100+a+b))**2]
            self.assertEqual(e.passed, a <= 50)
            for j in range(2):
                self.assertAlmostEqual(e.values[j], expected[j], delta=1e-12)
                cols[j].append(expected[j])
        for j in range(2):
            self.assertAlmostEqual(r.statistics[j].mean, statistics.mean(cols[j]), delta=1e-12)
            self.assertAlmostEqual(r.statistics[j].sample_variance, statistics.variance(cols[j]), delta=1e-12)
        json.dumps(r._asdict(), allow_nan=False)


if __name__ == '__main__':
    unittest.main()
