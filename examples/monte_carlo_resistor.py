"""Run the RF resistor tolerance example through the installed C/Python boundary.

Usage: python monte_carlo_resistor.py path/to/rfmodel_c.dll [--installed]
"""
import json
import math
from pathlib import Path
import statistics
import sys

library_path = str(Path(sys.argv.pop(1)).resolve())
if '--installed' in sys.argv:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
import rfmodel

lib = rfmodel.Library(library_path)
def evaluate(point):
    with lib.network() as network:
        for resistance in point:
            network.add(lib.ideal_rlc(1e9, element='resistor', connection='series', value=resistance))
        network.connect(1, 2)
        s = network.external_s([0, 3])
    return abs(s[1][0])**2, abs(s[0][0])**2

result = lib.run_monte_carlo(
    [rfmodel.MonteCarloParameter('uniform', 45, 55), rfmodel.MonteCarloParameter('fixed', 50)],
    2, evaluate, samples=4096, seed=20261012,
    criteria=[rfmodel.MonteCarloLimits(lower=.25), rfmodel.MonteCarloLimits(upper=.25)])
assert result.stop == 'completed' and result.accepted_samples == result.random_words == 4096
assert result.passed_samples == 2060 and result.passed_fraction == result.valid_pass_fraction == 2060/4096
errors = []
columns = [[], []]
for e in result.history:
    a, b = e.parameters
    expected = [(100/(100+a+b))**2, ((a+b)/(100+a+b))**2]
    assert e.passed == (a <= 50)
    for j in range(2):
        errors.append(abs(e.values[j]-expected[j]))
        columns[j].append(expected[j])
assert max(errors) < 1e-12
for j in range(2):
    assert abs(result.statistics[j].mean-statistics.mean(columns[j])) < 1e-12
    assert abs(result.statistics[j].sample_variance-statistics.variance(columns[j])) < 1e-12
population_mean = 1000*(1/195-1/205)
population_variance = 1e7/3*(195**-3-205**-3)-population_mean**2
assert abs(result.statistics[0].mean-population_mean) <= 6*math.sqrt(population_variance/4096)
report = {
    'format': 'rfmodel.monte-carlo-api-example', 'version': 1,
    'parameters': [{'distribution': 'uniform', 'first': 45, 'second': 55},
                   {'distribution': 'fixed', 'first': 50, 'second': 0}],
    'frequency_hz': 1e9, 'reference_ohms': 50,
    'metric_order': ['transmitted_power_fraction', 'reflected_power_fraction'],
    'criteria': [{'lower': .25}, {'upper': .25}],
    'options': {'samples': 4096, 'seed': 20261012, 'quantile_levels': [.05, .5, .95],
                'record_history': True, 'invalid_sample': 'stop'},
    'result': dict(result._asdict(), statistics=[s._asdict() for s in result.statistics],
                   history=[e._asdict() for e in result.history]),
    'max_independent_trace_error': max(errors),
    'analytic_population_gain_mean': population_mean,
    'analytic_population_gain_variance': population_variance,
    'analytic_pass_probability': .5,
}
print(json.dumps(report, indent=2, allow_nan=False))
