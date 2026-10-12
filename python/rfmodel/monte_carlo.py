"""Synchronous native Monte Carlo sampling and immutable Python result snapshots."""
import ctypes as ct
import itertools
import math
from typing import NamedTuple, Optional


class MonteCarloParameter(NamedTuple):
    distribution: str
    first: float
    second: float = 0.


class MonteCarloLimits(NamedTuple):
    lower: Optional[float] = None
    upper: Optional[float] = None


class MonteCarloStatistics(NamedTuple):
    count: int
    mean: Optional[float]
    minimum: Optional[float]
    maximum: Optional[float]
    sample_variance: Optional[float]
    sample_standard_deviation: Optional[float]
    standard_error: Optional[float]
    quantiles: tuple


class MonteCarloSample(NamedTuple):
    index: int
    parameters: tuple
    values: tuple
    status: str
    error: str
    passed: Optional[bool]


class MonteCarloResult(NamedTuple):
    algorithm: str
    seed: int
    random_words: int
    requested_samples: int
    samples_attempted: int
    evaluations: int
    accepted_samples: int
    rejected_samples: int
    stop: str
    has_statistics: bool
    criteria_enabled: bool
    passed_samples: int
    passed_fraction: Optional[float]
    valid_pass_fraction: Optional[float]
    quantile_levels: tuple
    statistics: tuple
    last_failed_parameters: tuple
    last_error: str
    history: tuple


class MonteCarloJointStatistics(NamedTuple):
    dimension: int
    count: int
    sample_covariance: tuple
    sample_correlation: tuple


class MonteCarloExtendedResult(NamedTuple):
    result: MonteCarloResult
    latent_rank: int
    effective_latent_correlation: tuple
    parameter_joint: MonteCarloJointStatistics
    metric_joint: MonteCarloJointStatistics


class _Extension(ct.Structure):
    _fields_ = [('latent_correlation', ct.POINTER(ct.c_double)),
                ('correlation_count', ct.c_size_t), ('joint_statistics', ct.c_int)]


class _Dependence(ct.Structure):
    _fields_ = [('correlation_count', ct.c_size_t), ('latent_rank', ct.c_size_t),
                ('algorithm', ct.c_int), ('joint_statistics', ct.c_int)]


class _JointStatistics(ct.Structure):
    _fields_ = [('dimension', ct.c_size_t), ('count', ct.c_size_t)]


class _JointValue(ct.Structure):
    _fields_ = [('has_covariance', ct.c_int), ('has_correlation', ct.c_int),
                ('sample_covariance', ct.c_double), ('sample_correlation', ct.c_double)]


class _Parameter(ct.Structure):
    _fields_ = [('distribution', ct.c_int), ('first', ct.c_double), ('second', ct.c_double)]


class _Limits(ct.Structure):
    _fields_ = [('has_lower', ct.c_int), ('has_upper', ct.c_int),
                ('lower', ct.c_double), ('upper', ct.c_double)]


class _Options(ct.Structure):
    _fields_ = [('samples', ct.c_size_t), ('seed', ct.c_uint64),
                ('reject_invalid', ct.c_int), ('record_history', ct.c_int),
                ('quantile_levels', ct.POINTER(ct.c_double)), ('quantile_count', ct.c_size_t),
                ('criteria', ct.POINTER(_Limits)), ('criteria_count', ct.c_size_t)]


class _Summary(ct.Structure):
    _fields_ = [(name, ct.c_size_t) for name in (
        'parameter_count', 'metric_count', 'quantile_count', 'history_count',
        'requested_samples', 'samples_attempted', 'evaluations', 'accepted_samples',
        'rejected_samples', 'passed_samples', 'last_failure_count', 'last_error_bytes'
    )] + [('seed', ct.c_uint64), ('random_words', ct.c_uint64)] + [
        (name, ct.c_int) for name in ('stop', 'has_statistics', 'criteria_enabled',
                                    'has_passed_fraction', 'has_valid_pass_fraction')
    ] + [('passed_fraction', ct.c_double), ('valid_pass_fraction', ct.c_double)]


class _Statistics(ct.Structure):
    _fields_ = [('count', ct.c_size_t), ('quantile_count', ct.c_size_t)] + [
        (name, ct.c_double) for name in ('mean', 'minimum', 'maximum')
    ] + [(name, ct.c_int) for name in ('has_sample_variance', 'has_sample_standard_deviation',
                                      'has_standard_error')] + [
        (name, ct.c_double) for name in ('sample_variance', 'sample_standard_deviation', 'standard_error')]


class _Sample(ct.Structure):
    _fields_ = [(name, ct.c_size_t) for name in ('index', 'value_count', 'error_bytes')
               ] + [('status', ct.c_int), ('passed', ct.c_int)]


_DOUBLE = ct.POINTER(ct.c_double)
_CHAR = ct.POINTER(ct.c_char)
_EVALUATE = ct.CFUNCTYPE(ct.c_int, _DOUBLE, ct.c_size_t, ct.c_void_p, _DOUBLE,
                        ct.c_size_t, ct.POINTER(ct.c_size_t), _CHAR, ct.c_size_t)
_CANCEL = ct.CFUNCTYPE(ct.c_int, ct.c_void_p)
_DISTRIBUTIONS = {name: i for i, name in enumerate((
    'fixed', 'uniform', 'log_uniform', 'normal', 'log_normal', 'discrete_uniform'))}
_STOPS = ('completed', 'sampling_failure', 'evaluation_failure', 'cancelled')
_STATUSES = ('accepted', 'sampling_failure', 'nonfinite_measurement',
             'invalid_measurement_count', 'exception')
ALGORITHM = 'mt19937_64-box-muller52-v1'
COPULA_ALGORITHM = 'mt19937_64-box-muller52-gaussian-copula-pivoted-v1'


def _signatures():
    handle, size = ct.c_void_p, ct.c_size_t
    return {
        'rfmodel_run_monte_carlo': (ct.c_int, [ct.POINTER(_Parameter), size, size,
            ct.POINTER(_Options), _EVALUATE, _CANCEL, handle, ct.POINTER(handle)]),
        'rfmodel_run_monte_carlo_extended': (ct.c_int, [ct.POINTER(_Parameter), size, size,
            ct.POINTER(_Options), ct.POINTER(_Extension), _EVALUATE, _CANCEL, handle, ct.POINTER(handle)]),
        'rfmodel_monte_carlo_get_dependence': (ct.c_int,
            [handle, ct.POINTER(_Dependence), _DOUBLE, size]),
        'rfmodel_monte_carlo_get_joint_statistics': (ct.c_int,
            [handle, ct.c_int, ct.POINTER(_JointStatistics), ct.POINTER(_JointValue), size]),
        'rfmodel_monte_carlo_destroy': (None, [handle]),
        'rfmodel_monte_carlo_get_summary': (ct.c_int, [handle, ct.POINTER(_Summary)]),
        'rfmodel_monte_carlo_get_quantile_levels': (ct.c_int, [handle, _DOUBLE, size]),
        'rfmodel_monte_carlo_get_statistics': (ct.c_int,
            [handle, size, ct.POINTER(_Statistics), _DOUBLE, size]),
        'rfmodel_monte_carlo_get_last_failure': (ct.c_int, [handle, _DOUBLE, size, _CHAR, size]),
        'rfmodel_monte_carlo_get_sample': (ct.c_int,
            [handle, size, ct.POINTER(_Sample), _DOUBLE, size, _DOUBLE, size, _CHAR, size]),
    }


def _real(value):
    if isinstance(value, (bool, str, bytes)):
        raise ValueError('Expected a real numeric Monte Carlo value')
    return float(value)


def _integer(value, lower, upper, name):
    if type(value) is not int or not lower <= value <= upper:
        raise ValueError(f'{name} must be an integer in {lower}..{upper}')
    return value


def _items(values, maximum, name):
    values = list(itertools.islice(iter(values), maximum + 1))
    if len(values) > maximum:
        raise ValueError(f'Too many {name}; maximum {maximum}')
    return values


def _parameter(value):
    if isinstance(value, MonteCarloParameter):
        value = value._asdict()
    if (not isinstance(value, dict) or not {'distribution', 'first'} <= value.keys()
            or value.keys() - {'distribution', 'first', 'second'}):
        raise ValueError('Parameter requires distribution, first and optional second')
    kind = value['distribution']
    if not isinstance(kind, str) or kind not in _DISTRIBUTIONS:
        raise ValueError('Unknown Monte Carlo distribution')
    return _Parameter(_DISTRIBUTIONS[kind], _real(value['first']), _real(value.get('second', 0.)))


def _limits(value):
    if isinstance(value, MonteCarloLimits):
        value = value._asdict()
    if not isinstance(value, dict) or value.keys() - {'lower', 'upper'}:
        raise ValueError('Each criterion requires optional lower and upper limits')
    low, high = value.get('lower'), value.get('upper')
    return _Limits(low is not None, high is not None,
                   0. if low is None else _real(low), 0. if high is None else _real(high))


def _finite_or_none(value):
    return value if math.isfinite(value) else None


def _run(library, parameters, metric_count, evaluate, *, samples=1000, seed=0,
                    invalid_sample='stop', record_history=True, quantile_levels=(.05, .5, .95),
                    criteria=(), cancelled=None, _extension=None):
    """Sample using the native engine once; reads never call the evaluator again.

    Ordinary evaluator exceptions become failed samples. Fatal exceptions and
    cancellation hook exceptions are re-raised after releasing the native handle.
    Unavailable statistics and nonfinite failed parameters become None so results
    support strict JSON. Failed finite parameters (including underflowed zero)
    remain visible. With criteria disabled each sample's passed field is None.
    """
    parameters = _items(parameters, 64, 'parameters')
    if not parameters:
        raise ValueError('Expected 1..64 Monte Carlo parameters')
    metric_count = _integer(metric_count, 1, 64, 'metric_count')
    samples = _integer(samples, 1, 100000, 'samples')
    seed = _integer(seed, 0, 2**64-1, 'seed')
    if not callable(evaluate) or (cancelled is not None and not callable(cancelled)):
        raise TypeError('Evaluator and optional cancellation hook must be callable')
    if type(record_history) is not bool or invalid_sample not in ('stop', 'reject'):
        raise ValueError('Invalid history or failed-sample policy')
    native = (_Parameter * len(parameters))(*[_parameter(p) for p in parameters])
    quantiles = _items(quantile_levels, 256, 'quantile levels')
    quantiles = (ct.c_double * len(quantiles))(*[_real(q) for q in quantiles])
    criteria = _items(criteria, 64, 'criteria')
    criteria = (_Limits * len(criteria))(*[_limits(c) for c in criteria])
    options = _Options(samples, seed, invalid_sample == 'reject', record_history,
                       quantiles, len(quantiles), criteria, len(criteria))
    extension = None
    if _extension is not None:
        correlation, joint = _extension
        if type(joint) is not bool:
            raise ValueError('joint_statistics must be bool')
        if isinstance(correlation, (str, bytes)):
            raise ValueError('latent_correlation must be a flat numeric iterable')
        correlation = _items(correlation, len(parameters)**2, 'latent correlation entries')
        if len(correlation) not in (0, len(parameters)**2):
            raise ValueError('latent correlation dimension mismatch')
        correlation = (ct.c_double * len(correlation))(*[_real(x) for x in correlation])
        extension = _Extension(correlation, len(correlation), joint)
    fatal = []

    @_EVALUATE
    def evaluator(point, count, context, output, capacity, value_count, error, error_capacity):
        try:
            values = list(itertools.islice(iter(evaluate(tuple(point[i] for i in range(count)))), capacity + 1))
            value_count[0] = len(values)
            if len(values) == capacity:
                for i, value in enumerate(values):
                    output[i] = _real(value)
            return 0
        except BaseException as exception:
            if isinstance(exception, MemoryError) or not isinstance(exception, Exception):
                fatal.append(exception)
            try:
                message = (type(exception).__name__ + ': ' + str(exception)).encode('utf-8')
                message = message[:max(0, error_capacity-1)].decode('utf-8', errors='ignore').encode('utf-8')
                if error_capacity:
                    ct.memmove(error, message + b'\0', len(message)+1)
            except BaseException as formatting_error:
                fatal.append(formatting_error)
            return 1

    @_CANCEL
    def cancel(context):
        if fatal:
            return 1
        if cancelled is None:
            return 0
        try:
            value = cancelled()
            if type(value) is not bool:
                raise TypeError('Cancellation callback must return bool')
            return int(value)
        except BaseException as exception:
            fatal.append(exception)
            return -1

    handle = ct.c_void_p()
    try:
        if extension is None:
            status = library._dll.rfmodel_run_monte_carlo(native, len(native), metric_count,
                ct.byref(options), evaluator, cancel, None, ct.byref(handle))
        else:
            status = library._dll.rfmodel_run_monte_carlo_extended(native, len(native), metric_count,
                ct.byref(options), ct.byref(extension), evaluator, cancel, None, ct.byref(handle))
        if fatal:
            raise fatal[0]
        library._check(status)
        summary = _Summary()
        library._check(library._dll.rfmodel_monte_carlo_get_summary(handle, ct.byref(summary)))
        levels = (ct.c_double * summary.quantile_count)()
        library._check(library._dll.rfmodel_monte_carlo_get_quantile_levels(handle, levels, len(levels)))
        statistics = []
        for j in range(summary.metric_count):
            info = _Statistics()
            library._check(library._dll.rfmodel_monte_carlo_get_statistics(handle, j, ct.byref(info), None, 0))
            values = (ct.c_double * info.quantile_count)()
            library._check(library._dll.rfmodel_monte_carlo_get_statistics(handle, j, ct.byref(info), values, len(values)))
            statistics.append(MonteCarloStatistics(info.count,
                *[_finite_or_none(getattr(info, name)) for name in ('mean', 'minimum', 'maximum')],
                *[getattr(info, name) if getattr(info, 'has_' + name) else None
                  for name in ('sample_variance', 'sample_standard_deviation', 'standard_error')], tuple(values)))
        failed = (ct.c_double * summary.last_failure_count)()
        error = ct.create_string_buffer(summary.last_error_bytes + 1)
        library._check(library._dll.rfmodel_monte_carlo_get_last_failure(handle, failed, len(failed), error, len(error)))
        history = []
        for i in range(summary.history_count):
            info = _Sample()
            library._check(library._dll.rfmodel_monte_carlo_get_sample(handle, i, ct.byref(info), None, 0, None, 0, None, 0))
            point = (ct.c_double * summary.parameter_count)()
            values = (ct.c_double * info.value_count)()
            message = ct.create_string_buffer(info.error_bytes + 1)
            library._check(library._dll.rfmodel_monte_carlo_get_sample(handle, i, ct.byref(info),
                point, len(point), values, len(values), message, len(message)))
            history.append(MonteCarloSample(info.index, tuple(_finite_or_none(x) for x in point),
                tuple(values), _STATUSES[info.status], message.value.decode('utf-8', errors='replace'),
                bool(info.passed) if summary.criteria_enabled else None))
        algorithm = ALGORITHM
        if extension is not None:
            dependence = _Dependence()
            library._check(library._dll.rfmodel_monte_carlo_get_dependence(handle, ct.byref(dependence), None, 0))
            effective = (ct.c_double * dependence.correlation_count)()
            library._check(library._dll.rfmodel_monte_carlo_get_dependence(handle, ct.byref(dependence), effective, len(effective)))
            algorithm = (ALGORITHM, COPULA_ALGORITHM)[dependence.algorithm]
            joints = []
            for parameters_matrix in (1, 0):
                meta = _JointStatistics()
                library._check(library._dll.rfmodel_monte_carlo_get_joint_statistics(handle, parameters_matrix, ct.byref(meta), None, 0))
                entries = (_JointValue * (meta.dimension**2))()
                library._check(library._dll.rfmodel_monte_carlo_get_joint_statistics(handle, parameters_matrix, ct.byref(meta), entries, len(entries)))
                joints.append(MonteCarloJointStatistics(meta.dimension, meta.count,
                    tuple(v.sample_covariance if v.has_covariance else None for v in entries),
                    tuple(v.sample_correlation if v.has_correlation else None for v in entries)))
        result = MonteCarloResult(algorithm, summary.seed, summary.random_words,
            summary.requested_samples, summary.samples_attempted, summary.evaluations,
            summary.accepted_samples, summary.rejected_samples, _STOPS[summary.stop],
            bool(summary.has_statistics), bool(summary.criteria_enabled), summary.passed_samples,
            summary.passed_fraction if summary.has_passed_fraction else None,
            summary.valid_pass_fraction if summary.has_valid_pass_fraction else None,
            tuple(levels), tuple(statistics), tuple(_finite_or_none(x) for x in failed),
            error.value.decode('utf-8', errors='replace'), tuple(history))
        if extension is None:
            return result
        return MonteCarloExtendedResult(result, dependence.latent_rank, tuple(effective), *joints)
    finally:
        if handle.value:
            library._dll.rfmodel_monte_carlo_destroy(handle)


def run_monte_carlo(library, parameters, metric_count, evaluate, *, samples=1000, seed=0,
                    invalid_sample='stop', record_history=True, quantile_levels=(.05, .5, .95),
                    criteria=(), cancelled=None):
    """Original independent API; preserves the 19-field MonteCarloResult layout."""
    return _run(library, parameters, metric_count, evaluate, samples=samples, seed=seed,
        invalid_sample=invalid_sample, record_history=record_history, quantile_levels=quantile_levels,
        criteria=criteria, cancelled=cancelled)


def run_monte_carlo_extended(library, parameters, metric_count, evaluate, *, samples=1000, seed=0,
                             invalid_sample='stop', record_history=True, quantile_levels=(.05, .5, .95),
                             criteria=(), cancelled=None, latent_correlation=(), joint_statistics=False):
    """Native copula and optional paired statistics; returns MonteCarloExtendedResult.

    latent_correlation is a flat row-major matrix in parameter order. Empty uses
    the original independent stream. Joint matrices contain only accepted rows.
    The original result is available at .result; all snapshots are immutable.
    """
    return _run(library, parameters, metric_count, evaluate, samples=samples, seed=seed,
        invalid_sample=invalid_sample, record_history=record_history, quantile_levels=quantile_levels,
        criteria=criteria, cancelled=cancelled, _extension=(latent_correlation, joint_statistics))
