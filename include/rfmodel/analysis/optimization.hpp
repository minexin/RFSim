#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rfmodel {
enum class OptimizationParameterKind {
    Real,
    LogReal,
    Integer
};
enum class OptimizationFailurePolicy {
    Stop,
    Reject
};
enum class OptimizationStop {
    TargetReached,
    StepToleranceReached,
    AllParametersFixed,
    EvaluationLimit,
    IterationLimit,
    ObjectiveFailure,
    NoFinitePoint,
    NumericalResolution,
    Cancelled
};
enum class OptimizationEvaluationStatus {
    Finite,
    NonFinite,
    Exception
};

struct OptimizationParameter {
    double lower{}, upper{}, initial{};
    OptimizationParameterKind kind{OptimizationParameterKind::Real};
};

struct OptimizationOptions {
    std::size_t max_evaluations{10000}, max_iterations{2000};
    double initial_step{.25}, minimum_step{1e-7}, contraction{.5};
    std::optional<double> target;
    OptimizationFailurePolicy invalid_evaluation{OptimizationFailurePolicy::Stop};
    bool record_history{true};
};

struct OptimizationEvaluation {
    std::vector<double> parameters;
    double value{std::numeric_limits<double>::quiet_NaN()};
    std::size_t iteration{};
    OptimizationEvaluationStatus status{OptimizationEvaluationStatus::Finite};
    std::string error;
};

struct OptimizationResult {
    std::vector<double> parameters;
    double value{std::numeric_limits<double>::quiet_NaN()};
    bool has_finite_point{};
    OptimizationStop stop{OptimizationStop::EvaluationLimit};
    std::size_t evaluations{}, rejected_evaluations{}, iterations{};
    double final_step{};
    std::vector<double> last_failed_parameters;
    std::string last_error;
    std::vector<OptimizationEvaluation> history;
};

inline const char *optimization_stop_name(OptimizationStop stop) {
    switch (stop) {
    case OptimizationStop::TargetReached:
        return "target_reached";
    case OptimizationStop::StepToleranceReached:
        return "step_tolerance_reached";
    case OptimizationStop::AllParametersFixed:
        return "all_parameters_fixed";
    case OptimizationStop::EvaluationLimit:
        return "evaluation_limit";
    case OptimizationStop::IterationLimit:
        return "iteration_limit";
    case OptimizationStop::ObjectiveFailure:
        return "objective_failure";
    case OptimizationStop::NoFinitePoint:
        return "no_finite_point";
    case OptimizationStop::NumericalResolution:
        return "numerical_resolution";
    case OptimizationStop::Cancelled:
        return "cancelled";
    }
    throw std::invalid_argument("invalid optimization stop code");
}

namespace optimization_detail {
inline void validate(const std::vector<OptimizationParameter> &parameters,
                     const OptimizationOptions &options) {
    if (parameters.empty() || parameters.size() > 64 || options.max_evaluations == 0 ||
        options.max_evaluations > 100000 || options.max_iterations == 0 ||
        options.max_iterations > 100000 || !std::isfinite(options.initial_step) ||
        !std::isfinite(options.minimum_step) || !std::isfinite(options.contraction) ||
        options.initial_step <= 0. || options.initial_step > 1. || options.minimum_step <= 0. ||
        options.minimum_step > options.initial_step || options.contraction <= 0. ||
        options.contraction >= 1. || (options.target && !std::isfinite(*options.target)) ||
        (options.invalid_evaluation != OptimizationFailurePolicy::Stop &&
         options.invalid_evaluation != OptimizationFailurePolicy::Reject)) {
        throw std::invalid_argument("invalid bounded optimization request or limits");
    }
    constexpr double integer_limit = 9007199254740991.;
    for (const auto &p : parameters) {
        if (!std::isfinite(p.lower) || !std::isfinite(p.upper) || !std::isfinite(p.initial) ||
            p.lower > p.upper || p.initial < p.lower || p.initial > p.upper) {
            throw std::invalid_argument("optimization initial point must satisfy finite bounds");
        }
        switch (p.kind) {
        case OptimizationParameterKind::LogReal:
            if (p.lower <= 0.) {
                throw std::invalid_argument("log parameter bounds must be positive");
            }
            break;
        case OptimizationParameterKind::Integer:
            for (double value : {p.lower, p.upper, p.initial}) {
                if (std::abs(value) > integer_limit || std::trunc(value) != value) {
                    throw std::invalid_argument("integer parameters require exact safe integers");
                }
            }
            break;
        case OptimizationParameterKind::Real:
            if (!std::isfinite(p.upper - p.lower)) {
                throw std::invalid_argument("linear parameter range overflow");
            }
            break;
        default:
            throw std::invalid_argument("unknown optimization parameter kind");
        }
    }
}

inline double trial(const OptimizationParameter &p, double current, double step, int direction) {
    if (p.lower == p.upper) {
        return current;
    }
    if (p.kind == OptimizationParameterKind::Integer) {
        const auto distance = std::max(1., std::ceil(step * (p.upper - p.lower)));
        return std::clamp(current + direction * distance, p.lower, p.upper);
    }
    const bool logarithmic = p.kind == OptimizationParameterKind::LogReal;
    const double lower = logarithmic ? std::log(p.lower) : p.lower;
    const double upper = logarithmic ? std::log(p.upper) : p.upper;
    // Distinct endpoints may collapse after log rounding; caller reports resolution.
    if (lower == upper) {
        return current;
    }
    const double value = logarithmic ? std::log(current) : current;
    const double coordinate =
        std::clamp((value - lower) / (upper - lower) + direction * step, 0., 1.);
    if (coordinate == 0.) {
        return p.lower;
    }
    if (coordinate == 1.) {
        return p.upper;
    }
    const double candidate = lower + coordinate * (upper - lower);
    return std::clamp(logarithmic ? std::exp(candidate) : candidate, p.lower, p.upper);
}
} // namespace optimization_detail

// Deterministic complete coordinate polling within finite box bounds.
// A stop reason is not a claim of a global optimum. The objective must be
// deterministic for replay; evaluations are serial and are not cached.
inline OptimizationResult
minimize_bounded(const std::vector<OptimizationParameter> &parameters,
                 const std::function<double(const std::vector<double> &)> &objective,
                 const OptimizationOptions &options = {},
                 const std::function<bool()> &cancelled = {}) {
    optimization_detail::validate(parameters, options);
    if (!objective) {
        throw std::invalid_argument("missing optimization objective");
    }
    OptimizationResult result;
    result.final_step = options.initial_step;
    for (const auto &parameter : parameters) {
        result.parameters.push_back(parameter.initial);
    }
    // Returns false when evaluation must stop, retaining the best earlier point.
    const auto evaluate = [&](const std::vector<double> &point) {
        if (cancelled && cancelled()) {
            result.stop = OptimizationStop::Cancelled;
            return false;
        }
        if (result.evaluations == options.max_evaluations) {
            result.stop = OptimizationStop::EvaluationLimit;
            return false;
        }
        OptimizationEvaluation entry;
        entry.parameters = point;
        entry.iteration = result.iterations;
        ++result.evaluations;
        try {
            entry.value = objective(point);
            if (!std::isfinite(entry.value)) {
                entry.status = OptimizationEvaluationStatus::NonFinite;
                entry.error = "objective returned a nonfinite value";
            }
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &error) {
            entry.status = OptimizationEvaluationStatus::Exception;
            entry.error = std::string(error.what()).substr(0, 512);
        } catch (...) {
            entry.status = OptimizationEvaluationStatus::Exception;
            entry.error = "objective threw a non-standard exception";
        }
        const bool valid = entry.status == OptimizationEvaluationStatus::Finite;
        if (valid && (!result.has_finite_point || entry.value < result.value)) {
            result.has_finite_point = true;
            result.parameters = point;
            result.value = entry.value;
        }
        if (!valid) {
            ++result.rejected_evaluations;
            result.last_failed_parameters = point;
            result.last_error = entry.error;
        }
        if (options.record_history) {
            result.history.push_back(std::move(entry));
        }
        if (!valid && options.invalid_evaluation == OptimizationFailurePolicy::Stop) {
            result.stop = OptimizationStop::ObjectiveFailure;
            return false;
        }
        if (result.has_finite_point && options.target && result.value <= *options.target) {
            result.stop = OptimizationStop::TargetReached;
            return false;
        }
        return true;
    };
    // Avoid aliasing result.parameters while evaluate updates the incumbent.
    const auto initial = result.parameters;
    if (!evaluate(initial)) {
        return result;
    }
    const bool fixed = std::all_of(parameters.begin(), parameters.end(), [](const auto &p) {
        return p.lower == p.upper;
    });
    if (fixed) {
        result.stop = result.has_finite_point ? OptimizationStop::AllParametersFixed
                                              : OptimizationStop::NoFinitePoint;
        return result;
    }
    for (std::size_t iteration = 0; iteration < options.max_iterations; ++iteration) {
        if (cancelled && cancelled()) {
            result.stop = OptimizationStop::Cancelled;
            return result;
        }
        ++result.iterations;
        const auto center = result.parameters;
        const double previous = result.value;
        const bool previously_finite = result.has_finite_point;
        bool attempted = false;
        for (std::size_t axis = 0; axis < parameters.size(); ++axis) {
            for (int direction : {1, -1}) {
                auto point = center;
                point[axis] = optimization_detail::trial(
                    parameters[axis], center[axis], result.final_step, direction);
                if (point == center) {
                    continue;
                }
                attempted = true;
                if (!evaluate(point)) {
                    return result;
                }
            }
        }
        const bool improved =
            result.has_finite_point && (!previously_finite || result.value < previous);
        if (!improved) {
            if (!attempted) {
                result.stop = result.has_finite_point ? OptimizationStop::NumericalResolution
                                                      : OptimizationStop::NoFinitePoint;
                return result;
            }
            if (result.final_step <= options.minimum_step) {
                result.stop = result.has_finite_point ? OptimizationStop::StepToleranceReached
                                                      : OptimizationStop::NoFinitePoint;
                return result;
            }
            const double next =
                std::max(options.minimum_step, result.final_step * options.contraction);
            if (next == result.final_step) {
                result.stop = result.has_finite_point ? OptimizationStop::NumericalResolution
                                                      : OptimizationStop::NoFinitePoint;
                return result;
            }
            result.final_step = next;
        }
    }
    result.stop = OptimizationStop::IterationLimit;
    return result;
}
} // namespace rfmodel
