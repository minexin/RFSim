"""Declarative tolerance tasks using native Monte Carlo and existing RF analyses."""
import copy
import json
from . import MonteCarloParameter, MonteCarloLimits
from .analysis_file import analysis_operation
from .model_file import _object, _number
from .parameter_binding import (
    _COMPONENTS, _array, _name, _path, _resolve, _assign, _bound_value, _fingerprints, _measurement,
)

_DEFAULTS = dict(samples=1000, seed=0, invalid_sample='stop', record_history=True,
                 quantile_levels=[.05, .5, .95])


def _distribution(document):
    if not isinstance(document, dict) or not isinstance(document.get('kind'), str):
        raise ValueError('Distribution must be an object with a kind string')
    kind = document['kind']
    fields = {
        'fixed': ('value',),
        'uniform': ('lower', 'upper'),
        'log_uniform': ('lower', 'upper'),
        'discrete_uniform': ('lower', 'upper'),
        'normal': ('mean', 'standard_deviation'),
        'log_normal': ('log_mean', 'log_standard_deviation'),
    }
    if kind not in fields:
        raise ValueError('Unsupported Monte Carlo distribution: ' + kind)
    _object(document, ('kind',) + fields[kind])
    values = [_number(document[key]) for key in fields[kind]]
    encoded = dict(kind=kind, **dict(zip(fields[kind], values)))
    parameter = MonteCarloParameter(kind, values[0], values[1] if len(values) == 2 else 0.)
    # Unbounded normal/log-normal mappings are checked per sample, not clipped.
    support = None if kind in ('normal', 'log_normal') else (values[0], values[-1])
    return encoded, parameter, support


def analyze_monte_carlo(library, document, *, base_directory=None, cancelled=None):
    """Copy a task, bind each sample once, and return metrics/statistics without reruns.

    evaluations counts all native evaluator calls; analysis_evaluations counts
    calls that got past numeric binding and entered the chosen RF analyzer.
    Shared parameter bindings reuse the same sample, preserving their dependency.
    """
    _object(document, ('format', 'version', 'analysis', 'parameters', 'metrics'), ('options',))
    if document['format'] != 'rfmodel.monte-carlo' or type(document['version']) is not int or document['version'] != 1:
        raise ValueError('Unsupported Monte Carlo format/version')
    # Task snapshots must remain serializable even when individual samples fail.
    json.dumps(document, allow_nan=False)
    template = copy.deepcopy(document['analysis'])
    operation = analysis_operation(template)
    options = document.get('options', {})
    _object(options, (), _DEFAULTS)
    options = copy.deepcopy(dict(_DEFAULTS, **options))
    if not isinstance(options['quantile_levels'], list):
        raise ValueError('quantile_levels must be a JSON array')
    parameter_specs, native, bindings = [], [], []
    ids, destinations = set(), set()
    for spec in _array(document['parameters'], 64, 'Parameters'):
        _object(spec, ('id', 'distribution', 'bindings'), ('unit',))
        name = _name(spec['id'])
        if name in ids:
            raise ValueError('Duplicate parameter id: ' + name)
        ids.add(name)
        distribution, parameter, support = _distribution(spec['distribution'])
        native.append(parameter)
        encoded = dict(id=name, distribution=distribution, bindings=[])
        if 'unit' in spec:
            encoded['unit'] = _name(spec['unit'])
        compiled = []
        for binding in _array(spec['bindings'], 256, 'Bindings'):
            _object(binding, ('path',), ('scale', 'offset'))
            path = _path(binding['path'])
            old, canonical = _resolve(template, path)
            _number(old)
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
            integral = parameter.distribution == 'discrete_uniform' and scale.is_integer() and offset.is_integer()
            if support is not None:
                for endpoint in support:
                    _bound_value(endpoint, scale, offset, integral)
            compiled.append((canonical, scale, offset, integral))
            encoded['bindings'].append(dict(path=path, scale=scale, offset=offset))
        parameter_specs.append(encoded)
        bindings.append(compiled)
    metrics, criteria, ids = [], [], set()
    has_limits = False
    for spec in _array(document['metrics'], 64, 'Metrics'):
        _object(spec, ('id', 'path'), ('component', 'limits', 'unit'))
        name = _name(spec['id'])
        if name in ids:
            raise ValueError('Duplicate metric id: ' + name)
        ids.add(name)
        component = spec.get('component', 'value')
        if component not in _COMPONENTS:
            raise ValueError('Unsupported measurement component')
        entry = dict(id=name, path=_path(spec['path']), component=component)
        if 'unit' in spec:
            entry['unit'] = _name(spec['unit'])
        limits = {}
        if 'limits' in spec:
            _object(spec['limits'], (), ('lower', 'upper'))
            if not spec['limits']:
                raise ValueError('Explicit limits must contain at least one bound')
            limits = {key: _number(value) for key, value in spec['limits'].items()}
            entry['limits'] = limits
            has_limits = True
        criteria.append(MonteCarloLimits(limits.get('lower'), limits.get('upper')))
        metrics.append(entry)
    dependencies = _fingerprints(template, base_directory)
    analysis_evaluations = 0

    def evaluate(point):
        nonlocal analysis_evaluations
        candidate = copy.deepcopy(template)
        for value, compiled in zip(point, bindings):
            for path, scale, offset, integral in compiled:
                _assign(candidate, path, _bound_value(value, scale, offset, integral))
        analysis_evaluations += 1
        analysis = operation(library, candidate, base_directory=base_directory)
        return [_number(_measurement(_resolve(analysis, m['path'])[0], m['component'])) for m in metrics]

    result = library.run_monte_carlo(native, len(metrics), evaluate, criteria=criteria if has_limits else (),
                                      cancelled=cancelled, **options)
    if _fingerprints(template, base_directory) != dependencies:
        raise RuntimeError('Touchstone input changed during Monte Carlo analysis')
    encoded_result = result._asdict()
    encoded_result['statistics'] = [entry._asdict() for entry in result.statistics]
    encoded_result['history'] = [entry._asdict() for entry in result.history]
    return dict(format='rfmodel.monte-carlo-results', version=1, algorithm=result.algorithm,
                parameter_order=[p['id'] for p in parameter_specs], parameters=parameter_specs,
                metric_order=[m['id'] for m in metrics], metrics=metrics, options=options,
                input_analysis=template, dependencies=dependencies,
                analysis_evaluations=analysis_evaluations, result=encoded_result)
