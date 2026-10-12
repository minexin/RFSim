"""LC matching through installed Python/C ABI/native optimizer; writes a JSON trace."""
import json
import math
import sys
import rfmodel

lib = rfmodel.Library(sys.argv[1])
parameters = [rfmodel.OptimizationParameter(1e-10, 3e-8, 2e-9, 'log'),
              rfmodel.OptimizationParameter(1e-13, 1e-10, 1e-11, 'log')]
options = dict(max_evaluations=10000, max_iterations=2000, initial_step=.25,
               minimum_step=1e-9, contraction=.5, target=1e-14,
               invalid_evaluation='stop', record_history=True)

def objective(point):
    with lib.network() as network:
        network.add(lib.ideal_rlc(1e9, element='capacitor', connection='shunt', value=point[1]))
        network.add(lib.ideal_rlc(1e9, element='inductor', connection='series', value=point[0]))
        network.connect(1, 2)
        network.terminate(3, reflection=-1/3)
        return abs(network.external_s([0])[0][0])**2

result = lib.minimize_bounded(parameters, objective, **options)
omega = 2*math.pi*1e9
analytic = (25/omega, .02/omega)
errors = []
for entry in result.history:
    l, c = entry.parameters
    impedance = 1/(1/complex(25, omega*l)+complex(0, omega*c))
    errors.append(abs(entry.value-abs((impedance-50)/(impedance+50))**2))
if (result.stop != 'target_reached' or max(errors) > 1e-12 or
        any(abs(x/ref-1)>1e-6 for x, ref in zip(result.parameters, analytic))):
    raise RuntimeError('Independent LC reference verification failed')
output = result._asdict()
output['history'] = [entry._asdict() for entry in result.history]
print(json.dumps(dict(format='rfmodel.optimization-python-example', version=1,
    frequency_hz=1e9, source_reference_ohms=50., load_ohms=25.,
    parameter_order=['series_inductance_h', 'shunt_capacitance_f'],
    parameters=[p._asdict() for p in parameters], options=options,
    analytic_parameters=analytic, maximum_trace_reference_error=max(errors), result=output),
    indent=2, allow_nan=False))
