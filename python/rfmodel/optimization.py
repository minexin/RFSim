"""Synchronous callbacks into native bounded optimization; no Python search loop."""
import ctypes as ct
from typing import NamedTuple, Optional


class OptimizationParameter(NamedTuple):
    lower: float
    upper: float
    initial: float
    kind: str = "real"


class OptimizationEvaluation(NamedTuple):
    parameters: tuple
    value: Optional[float]
    iteration: int
    status: str
    error: str


class OptimizationResult(NamedTuple):
    parameters: tuple
    value: Optional[float]
    has_finite_point: bool
    stop: str
    evaluations: int
    rejected_evaluations: int
    iterations: int
    final_step: float
    last_failed_parameters: tuple
    last_error: str
    history: tuple


class _Parameter(ct.Structure):
    _fields_ = [("lower", ct.c_double), ("upper", ct.c_double),
                ("initial", ct.c_double), ("kind", ct.c_int)]


class _Options(ct.Structure):
    _fields_ = [("max_evaluations", ct.c_size_t), ("max_iterations", ct.c_size_t),
                ("initial_step", ct.c_double), ("minimum_step", ct.c_double),
                ("contraction", ct.c_double), ("target", ct.c_double),
                ("has_target", ct.c_int), ("reject_invalid", ct.c_int),
                ("record_history", ct.c_int)]


class _Summary(ct.Structure):
    _fields_ = [(name, ct.c_size_t) for name in (
        "parameter_count", "history_count", "evaluations", "rejected_evaluations",
        "iterations", "last_failure_count", "last_error_bytes"
    )] + [("has_finite_point", ct.c_int), ("stop", ct.c_int),
         ("value", ct.c_double), ("final_step", ct.c_double)]


class _Evaluation(ct.Structure):
    _fields_ = [("value", ct.c_double), ("iteration", ct.c_size_t),
                ("status", ct.c_int), ("error_bytes", ct.c_size_t)]


_DOUBLE = ct.POINTER(ct.c_double)
_CHAR = ct.POINTER(ct.c_char)
_OBJECTIVE = ct.CFUNCTYPE(ct.c_int, _DOUBLE, ct.c_size_t, ct.c_void_p,
                         _DOUBLE, _CHAR, ct.c_size_t)
_CANCEL = ct.CFUNCTYPE(ct.c_int, ct.c_void_p)
_STOPS = ("target_reached", "step_tolerance_reached", "all_parameters_fixed",
          "evaluation_limit", "iteration_limit", "objective_failure",
          "no_finite_point", "numerical_resolution", "cancelled")
_STATUSES = ("finite", "nonfinite", "exception")
_KINDS = {"real": 0, "log": 1, "integer": 2}


def _signatures():
    handle, size = ct.c_void_p, ct.c_size_t
    return {
        "rfmodel_optimize_bounded": (ct.c_int, [ct.POINTER(_Parameter), size,
            ct.POINTER(_Options), _OBJECTIVE, _CANCEL, handle, ct.POINTER(handle)]),
        "rfmodel_optimization_destroy": (None, [handle]),
        "rfmodel_optimization_get_summary": (ct.c_int, [handle, ct.POINTER(_Summary)]),
        "rfmodel_optimization_get_parameters": (ct.c_int, [handle, _DOUBLE, size]),
        "rfmodel_optimization_get_last_failure": (ct.c_int, [handle, _DOUBLE, size, _CHAR, size]),
        "rfmodel_optimization_get_evaluation": (ct.c_int,
            [handle, size, ct.POINTER(_Evaluation), _DOUBLE, size, _CHAR, size]),
    }


def _real(value):
    if isinstance(value, (bool, str, bytes)):
        raise ValueError("Expected a real numeric optimization value")
    return float(value)


def _limit(value):
    if type(value) is not int or not 1 <= value <= 100000:
        raise ValueError("Optimization budgets must be integers in 1..100000")
    return value


def _parameter(value):
    if isinstance(value, OptimizationParameter):
        value = value._asdict()
    required = {"lower", "upper", "initial"}
    if not isinstance(value, dict) or not required <= value.keys() or value.keys() - required - {"kind"}:
        raise ValueError("Optimization parameter requires lower, upper, initial and optional kind")
    kind = value.get("kind", "real")
    if not isinstance(kind, str) or kind not in _KINDS:
        raise ValueError("Parameter kind must be real, log or integer")
    return _Parameter(*(_real(value[name]) for name in ("lower", "upper", "initial")), _KINDS[kind])


def minimize_bounded(library, parameters, objective, *, max_evaluations=10000,
                     max_iterations=2000, initial_step=.25, minimum_step=1e-7,
                     contraction=.5, target=None, invalid_evaluation="stop",
                     record_history=True, cancelled=None):
    """Run native optimization once, then copy and release its immutable result.

    Ordinary objective exceptions become failed evaluations. MemoryError,
    KeyboardInterrupt, SystemExit and cancellation callback errors are re-raised
    after native control returns and its result handle is destroyed.
    """
    parameters = list(parameters)
    if not 1 <= len(parameters) <= 64:
        raise ValueError("Expected 1..64 optimization parameters")
    if not callable(objective) or (cancelled is not None and not callable(cancelled)):
        raise TypeError("Objective and optional cancellation hook must be callable")
    if type(record_history) is not bool or invalid_evaluation not in ("stop", "reject"):
        raise ValueError("Invalid history or failed-evaluation policy")
    native = (_Parameter * len(parameters))(*[_parameter(p) for p in parameters])
    options = _Options(_limit(max_evaluations), _limit(max_iterations),
                       _real(initial_step), _real(minimum_step), _real(contraction),
                       0. if target is None else _real(target), target is not None,
                       invalid_evaluation == "reject", record_history)
    fatal = []

    @_OBJECTIVE
    def evaluate(values, count, context, output, error, capacity):
        try:
            output[0] = _real(objective(tuple(values[i] for i in range(count))))
            return 0
        except BaseException as exception:
            if isinstance(exception, MemoryError) or not isinstance(exception, Exception):
                fatal.append(exception)
            try:
                message = (type(exception).__name__ + ": " + str(exception)).encode("utf-8")
                # Keep valid UTF-8 inside the native diagnostic byte limit.
                message = message[:max(0, capacity-1)].decode("utf-8", errors="ignore").encode("utf-8")
                if capacity:
                    ct.memmove(error, message + b"\0", len(message)+1)
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
                raise TypeError("Cancellation callback must return bool")
            return int(value)
        except BaseException as exception:
            fatal.append(exception)
            return -1

    handle = ct.c_void_p()
    try:
        status = library._dll.rfmodel_optimize_bounded(
            native, len(native), ct.byref(options), evaluate, cancel, None, ct.byref(handle))
        if fatal:
            raise fatal[0]
        library._check(status)
        summary = _Summary()
        library._check(library._dll.rfmodel_optimization_get_summary(handle, ct.byref(summary)))
        best = (ct.c_double * summary.parameter_count)()
        library._check(library._dll.rfmodel_optimization_get_parameters(handle, best, len(best)))
        failed = (ct.c_double * summary.last_failure_count)()
        error = ct.create_string_buffer(summary.last_error_bytes+1)
        library._check(library._dll.rfmodel_optimization_get_last_failure(
            handle, failed, len(failed), error, len(error)))
        last_error = error.value.decode("utf-8", errors="replace")
        history = []
        for index in range(summary.history_count):
            info = _Evaluation()
            point = (ct.c_double * summary.parameter_count)()
            message = ct.create_string_buffer(513)
            library._check(library._dll.rfmodel_optimization_get_evaluation(
                handle, index, ct.byref(info), point, len(point), message, len(message)))
            history.append(OptimizationEvaluation(tuple(point),
                info.value if info.status == 0 else None, info.iteration,
                _STATUSES[info.status], message.value.decode("utf-8", errors="replace")))
        return OptimizationResult(tuple(best), summary.value if summary.has_finite_point else None,
            bool(summary.has_finite_point), _STOPS[summary.stop], summary.evaluations,
            summary.rejected_evaluations, summary.iterations, summary.final_step,
            tuple(failed), last_error, tuple(history))
    finally:
        if handle.value:
            library._dll.rfmodel_optimization_destroy(handle)
