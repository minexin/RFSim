"""Declarative model parameter binding and native bounded optimization tasks."""
import copy
import math
from . import OptimizationParameter
from .analysis_file import analysis_operation
from .model_file import _object, _number
from .parameter_binding import (
    _COMPONENTS, _array, _name, _path, _resolve, _assign, _bound_value, _fingerprints, _measurement,
)

_DEFAULTS = dict(max_evaluations=10000, max_iterations=2000, initial_step=.25,
                 minimum_step=1e-7, contraction=.5, target=None,
                 invalid_evaluation='stop', record_history=True)
_GOALS = ('minimize', 'maximize', 'target', 'at_most', 'at_least')


def _objective_values(analysis, objectives):
    values = []
    for objective in objectives:
        raw, _ = _resolve(analysis, objective['path'])
        measured = _number(_measurement(raw, objective['component']))
        goal, scale = objective['goal'], objective['scale']
        if goal in ('minimize', 'maximize'):
            contribution = measured/scale * (1 if goal == 'minimize' else -1)
        else:
            difference = measured-objective['target']
            if goal == 'at_most':
                difference = max(0., difference)
            if goal == 'at_least':
                difference = min(0., difference)
            contribution = (difference/scale)**2
        values.append(dict(id=objective['id'], measured=measured,
                           contribution=_number(objective['weight']*contribution)))
    return _number(math.fsum(value['contribution'] for value in values)), values


def analyze_optimization(library, document, *, base_directory=None, cancelled=None):
    """Bind copied inputs, search once in C++, and return the best saved analysis.

    No final analysis is rerun outside the evaluation budget. External Touchstone
    inputs must remain unchanged; fingerprints before and after must agree.
    """
    _object(document, ('format', 'version', 'analysis', 'parameters', 'objectives'), ('options',))
    if document['format'] != 'rfmodel.optimization' or type(document['version']) is not int or document['version'] != 1:
        raise ValueError('Unsupported optimization format/version')
    template = copy.deepcopy(document['analysis'])
    operation = analysis_operation(template)
    options = document.get('options', {})
    _object(options, (), _DEFAULTS)
    options = dict(_DEFAULTS, **copy.deepcopy(options))
    parameter_specs, native, bindings = [], [], []
    ids, destinations = set(), set()
    for spec in _array(document['parameters'], 64, 'Parameters'):
        _object(spec, ('id', 'lower', 'upper', 'initial', 'bindings'), ('kind', 'unit'))
        name = _name(spec['id'])
        if name in ids:
            raise ValueError('Duplicate parameter id: ' + name)
        ids.add(name)
        kind = spec.get('kind', 'real')
        if kind not in ('real', 'log', 'integer'):
            raise ValueError('Parameter kind must be real, log or integer')
        lower, upper, initial = [_number(spec[key]) for key in ('lower', 'upper', 'initial')]
        native.append(OptimizationParameter(lower, upper, initial, kind))
        encoded = dict(id=name, lower=lower, upper=upper, initial=initial, kind=kind, bindings=[])
        if 'unit' in spec:
            encoded['unit'] = _name(spec['unit'])
        compiled = []
        for binding in _array(spec['bindings'], 256, 'Bindings'):
            _object(binding, ('path',), ('scale', 'offset'))
            path = _path(binding['path'])
            old, canonical = _resolve(template, path)
            _number(old)  # Only existing numeric leaves may be bound; never add model fields.
            if canonical[-1] == 'version':
                raise ValueError('Cannot bind a format version')
            if canonical in destinations:
                raise ValueError('Overlapping parameter bindings')
            destinations.add(canonical)
            if len(destinations) > 4096:
                raise ValueError('At most 4096 parameter bindings are supported')
            scale, offset = _number(binding.get('scale', 1.)), _number(binding.get('offset', 0.))
            if scale == 0.:
                raise ValueError('Binding scale must be nonzero')
            integral = kind == 'integer' and scale.is_integer() and offset.is_integer()
            for endpoint in (lower, upper, initial):
                _bound_value(endpoint, scale, offset, integral)
            compiled.append((canonical, scale, offset, integral))
            encoded['bindings'].append(dict(path=path, scale=scale, offset=offset))
        parameter_specs.append(encoded)
        bindings.append(compiled)
    objectives, ids = [], set()
    for spec in _array(document['objectives'], 256, 'Objectives'):
        _object(spec, ('id', 'path', 'goal'), ('component', 'target', 'weight', 'scale'))
        name, goal = _name(spec['id']), spec['goal']
        if name in ids:
            raise ValueError('Duplicate objective id: ' + name)
        ids.add(name)
        component = spec.get('component', 'value')
        if component not in _COMPONENTS or goal not in _GOALS:
            raise ValueError('Unsupported objective component or goal')
        entry = dict(id=name, path=_path(spec['path']), component=component, goal=goal,
                     weight=_number(spec.get('weight', 1.)), scale=_number(spec.get('scale', 1.)))
        if entry['weight'] <= 0 or entry['scale'] <= 0:
            raise ValueError('Objective weight and scale must be positive')
        targeted = goal in ('target', 'at_most', 'at_least')
        if targeted != ('target' in spec):
            raise ValueError('Only target/at_most/at_least goals require target')
        if targeted:
            entry['target'] = _number(spec['target'])
        objectives.append(entry)
    dependencies = _fingerprints(template, base_directory)
    best = None

    def evaluate(point):
        nonlocal best
        candidate = copy.deepcopy(template)
        for value, compiled in zip(point, bindings):
            for path, scale, offset, integral in compiled:
                _assign(candidate, path, _bound_value(value, scale, offset, integral))
        analysis = operation(library, candidate, base_directory=base_directory)
        value, measurements = _objective_values(analysis, objectives)
        if best is None or value < best['value']:
            best = dict(value=value, point=point, model=candidate, analysis=analysis, objectives=measurements)
        return value

    result = library.minimize_bounded(native, evaluate, cancelled=cancelled, **options)
    if _fingerprints(template, base_directory) != dependencies:
        raise RuntimeError('Touchstone input changed during optimization')
    if result.has_finite_point and (best is None or result.parameters != best['point'] or result.value != best['value']):
        raise RuntimeError('Native optimizer and saved analysis disagree')
    encoded_result = result._asdict()
    encoded_result['history'] = [entry._asdict() for entry in result.history]
    return dict(format='rfmodel.optimization-results', version=1, algorithm='bounded_coordinate_poll',
                parameter_order=[p['id'] for p in parameter_specs], parameters=parameter_specs,
                objectives=objectives, options=options, input_analysis=template, dependencies=dependencies,
                result=encoded_result, best_model=best['model'] if best else None,
                best_analysis=best['analysis'] if best else None,
                best_objectives=best['objectives'] if best else None)
