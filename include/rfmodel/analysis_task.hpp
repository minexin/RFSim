#pragma once
#include "device_model.hpp"
#include "monte_carlo.hpp"
#include "optimization.hpp"
#include <memory>
#include <set>

namespace rfmodel {

// Numeric slots are a host-defined schema, not JSON paths. An assignment has one
// canonical slot index, so aliases/overlapping variable bindings are rejected.
struct AnalysisParameter {
    std::string name;
    double value{};
    std::string unit;
};

struct ParameterAssignment {
    std::size_t slot{};
    double scale{1.}, offset{};
};

struct OptimizationVariable {
    std::string name;
    OptimizationParameter parameter;
    std::vector<ParameterAssignment> assignments;
};

struct MonteCarloVariable {
    std::string name;
    MonteCarloParameter parameter;
    std::vector<ParameterAssignment> assignments;
};
enum class MeasurementProjection {
    value,
    real,
    imaginary,
    magnitude,
    power,
    magnitude_db,
    phase_radians
};
enum class ObjectiveGoal {
    minimize,
    maximize,
    target,
    at_most,
    at_least
};

template <class Result> struct ParameterizedAnalysis {
    std::vector<AnalysisParameter> parameters;
    std::function<Result(const std::vector<double> &)> evaluate;
    // Optional host revisions/hashes for externally owned dependencies. The
    // callback is checked before/after the task. Prefer captured immutable model
    // data (e.g. an imported Touchstone table) to reopening changing files.
    std::function<std::vector<std::string>()> dependency_revisions;
};

template <class Result> struct AnalysisMeasurement {
    std::string name;
    std::function<Complex(const Result &)> read;
    MeasurementProjection projection{MeasurementProjection::value};
};

template <class Result> struct AnalysisObjective {
    AnalysisMeasurement<Result> measurement;
    ObjectiveGoal goal{ObjectiveGoal::minimize};
    double weight{1.}, scale{1.};
    std::optional<double> target;
};

struct ObjectiveMeasurement {
    std::string name;
    double measured{}, contribution{};
};

template <class Result> struct BestAnalysis {
    std::vector<double> variable_values, bound_parameters;
    std::shared_ptr<const Result> analysis;
    std::vector<ObjectiveMeasurement> objectives;
    double value{};
};

template <class Result> struct OptimizationTaskResult {
    OptimizationResult search;
    std::optional<BestAnalysis<Result>> best;
    std::size_t analysis_evaluations{};
    std::vector<std::string> dependency_revisions;
};

struct MonteCarloTaskResult {
    MonteCarloResult sampling;
    std::size_t analysis_evaluations{};
    std::vector<std::string> metric_names, dependency_revisions;
};

namespace analysis_task_detail {
inline void name(const std::string &value) {
    if (value.empty() || value.size() > 1024 || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("invalid analysis task name");
    }
}

inline double finite(double value) {
    if (!std::isfinite(value)) {
        throw std::overflow_error("nonfinite analysis task value");
    }
    return value;
}

inline bool integral(double value) {
    return std::isfinite(value) && std::trunc(value) == value;
}

inline double bound_value(double value, const ParameterAssignment &binding, bool integer) {
    if (!integer) {
        return finite(value * binding.scale + binding.offset);
    }
    constexpr double limit = 9007199254740991.;
    for (double item : {value, binding.scale, binding.offset}) {
        if (!integral(item) || std::abs(item) > limit) {
            throw std::overflow_error("integer task binding exceeds exact integer range");
        }
    }
    const auto a = static_cast<std::int64_t>(value);
    const auto b = static_cast<std::int64_t>(binding.scale);
    const auto c = static_cast<std::int64_t>(binding.offset);
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    // |a|,|b| <= 2^53-1; no signed-min negation. If the product exceeds int64,
    // an offset limited to 2^53-1 cannot bring it back into the allowed domain.
    if (b != 0 && std::abs(a) > max / std::abs(b)) {
        throw std::overflow_error("integer task binding multiplication overflow");
    }
    const auto product = a * b;
    if ((c > 0 && product > max - c) ||
        (c < 0 && product < std::numeric_limits<std::int64_t>::min() - c)) {
        throw std::overflow_error("integer task binding addition overflow");
    }
    const auto result = product + c;
    if (result > static_cast<std::int64_t>(limit) || result < -static_cast<std::int64_t>(limit)) {
        throw std::overflow_error("integer task binding result exceeds exact integer range");
    }
    return static_cast<double>(result);
}

template <class Result>
inline std::vector<double> validate(const ParameterizedAnalysis<Result> &analysis) {
    if (!analysis.evaluate || analysis.parameters.empty() || analysis.parameters.size() > 4096) {
        throw std::invalid_argument(
            "analysis task requires 1..4096 numeric slots and an evaluator");
    }
    std::set<std::string> names;
    std::vector<double> initial;
    for (const auto &parameter : analysis.parameters) {
        name(parameter.name);
        if (!names.insert(parameter.name).second || parameter.unit.size() > 1024 ||
            parameter.unit.find('\0') != std::string::npos || !std::isfinite(parameter.value)) {
            throw std::invalid_argument("invalid or duplicate analysis parameter");
        }
        initial.push_back(parameter.value);
    }
    return initial;
}

template <class Variable>
inline void validate_bindings(const std::vector<Variable> &variables, std::size_t slots) {
    if (variables.empty() || variables.size() > 64) {
        throw std::invalid_argument("analysis task requires 1..64 variables");
    }
    std::set<std::string> names;
    std::set<std::size_t> assigned;
    for (const auto &variable : variables) {
        name(variable.name);
        if (!names.insert(variable.name).second || variable.assignments.empty() ||
            variable.assignments.size() > 256) {
            throw std::invalid_argument("invalid analysis task variable or assignments");
        }
        for (const auto &binding : variable.assignments) {
            if (binding.slot >= slots || !assigned.insert(binding.slot).second ||
                binding.scale == 0. || !std::isfinite(binding.scale) ||
                !std::isfinite(binding.offset)) {
                throw std::invalid_argument("unknown, overlapping or invalid parameter assignment");
            }
        }
    }
}

inline bool integer_variable(const OptimizationVariable &v) {
    return v.parameter.kind == OptimizationParameterKind::Integer;
}

inline bool integer_variable(const MonteCarloVariable &v) {
    return v.parameter.distribution == MonteCarloDistribution::DiscreteUniform;
}

template <class Variable>
inline std::vector<double> bind(const std::vector<double> &initial,
                                const std::vector<Variable> &variables,
                                const std::vector<double> &point) {
    if (point.size() != variables.size()) {
        throw std::invalid_argument("task point dimensions differ");
    }
    auto result = initial;
    for (std::size_t i = 0; i < point.size(); ++i) {
        for (const auto &assignment : variables[i].assignments) {
            const bool integer = integer_variable(variables[i]) && integral(assignment.scale) &&
                                 integral(assignment.offset);
            result[assignment.slot] = bound_value(point[i], assignment, integer);
        }
    }
    return result;
}

template <class Result> inline void validate(const AnalysisMeasurement<Result> &metric) {
    name(metric.name);
    if (!metric.read) {
        throw std::invalid_argument("missing analysis measurement reader");
    }
    switch (metric.projection) {
    case MeasurementProjection::value:
    case MeasurementProjection::real:
    case MeasurementProjection::imaginary:
    case MeasurementProjection::magnitude:
    case MeasurementProjection::power:
    case MeasurementProjection::magnitude_db:
    case MeasurementProjection::phase_radians:
        return;
    }
    throw std::invalid_argument("unknown analysis measurement projection");
}

inline double project(Complex value, MeasurementProjection projection) {
    finite(value.real());
    finite(value.imag());
    switch (projection) {
    case MeasurementProjection::value:
        if (value.imag() != 0.) {
            throw std::invalid_argument("scalar measurement must be real");
        }
        return value.real();
    case MeasurementProjection::real:
        return value.real();
    case MeasurementProjection::imaginary:
        return value.imag();
    case MeasurementProjection::magnitude:
        return finite(std::abs(value));
    case MeasurementProjection::power:
        return finite(std::norm(value));
    case MeasurementProjection::magnitude_db:
        return finite(20. * std::log10(std::abs(value)));
    case MeasurementProjection::phase_radians:
        return std::arg(value);
    }
    throw std::invalid_argument("unknown analysis measurement projection");
}

template <class Result>
inline void validate_objectives(const std::vector<AnalysisObjective<Result>> &objectives) {
    if (objectives.empty() || objectives.size() > 256) {
        throw std::invalid_argument("optimization task requires 1..256 objectives");
    }
    std::set<std::string> names;
    for (const auto &objective : objectives) {
        validate(objective.measurement);
        if (!names.insert(objective.measurement.name).second || !std::isfinite(objective.weight) ||
            objective.weight <= 0. || !std::isfinite(objective.scale) || objective.scale <= 0.) {
            throw std::invalid_argument("invalid optimization objective weight/scale/name");
        }
        bool targeted;
        switch (objective.goal) {
        case ObjectiveGoal::minimize:
        case ObjectiveGoal::maximize:
            targeted = false;
            break;
        case ObjectiveGoal::target:
        case ObjectiveGoal::at_most:
        case ObjectiveGoal::at_least:
            targeted = true;
            break;
        default:
            throw std::invalid_argument("unknown optimization objective goal");
        }
        if (targeted != objective.target.has_value() ||
            (objective.target && !std::isfinite(*objective.target))) {
            throw std::invalid_argument("optimization target required only for target/limit goals");
        }
    }
}

template <class Result>
inline std::pair<double, std::vector<ObjectiveMeasurement>>
measure(const Result &result, const std::vector<AnalysisObjective<Result>> &objectives) {
    std::vector<ObjectiveMeasurement> measured;
    double sum = 0., correction = 0.;
    for (const auto &objective : objectives) {
        const double value =
            project(objective.measurement.read(result), objective.measurement.projection);
        double contribution;
        if (objective.goal == ObjectiveGoal::minimize ||
            objective.goal == ObjectiveGoal::maximize) {
            contribution =
                value / objective.scale * (objective.goal == ObjectiveGoal::minimize ? 1. : -1.);
        } else {
            double difference = finite(value - *objective.target);
            if (objective.goal == ObjectiveGoal::at_most) {
                difference = std::max(0., difference);
            }
            if (objective.goal == ObjectiveGoal::at_least) {
                difference = std::min(0., difference);
            }
            difference = finite(difference / objective.scale);
            contribution = difference * difference;
        }
        contribution = finite(objective.weight * contribution);
        measured.push_back({objective.measurement.name, value, contribution});
        const double next = finite(sum + contribution);
        correction += std::abs(sum) >= std::abs(contribution) ? (sum - next) + contribution
                                                              : (contribution - next) + sum;
        sum = next;
    }
    return {finite(sum + correction), std::move(measured)};
}

template <class Result>
inline std::vector<std::string> revisions(const ParameterizedAnalysis<Result> &analysis) {
    return analysis.dependency_revisions ? analysis.dependency_revisions()
                                         : std::vector<std::string>{};
}
} // namespace analysis_task_detail

// Parameters, assignments, objectives and options are owned value snapshots.
// Callbacks run serially. Results never trigger an extra final RF evaluation.
template <class Result>
inline OptimizationTaskResult<Result>
run_optimization_task(ParameterizedAnalysis<Result> analysis,
                      std::vector<OptimizationVariable> variables,
                      std::vector<AnalysisObjective<Result>> objectives,
                      OptimizationOptions options = {},
                      const std::function<bool()> &cancelled = {}) {
    using namespace analysis_task_detail;
    const auto initial = validate(analysis);
    validate_bindings(variables, initial.size());
    validate_objectives(objectives);
    std::vector<OptimizationParameter> native;
    for (const auto &variable : variables) {
        native.push_back(variable.parameter);
    }
    optimization_detail::validate(native, options);
    for (const auto &variable : variables) {
        for (const auto &assignment : variable.assignments) {
            for (double value :
                 {variable.parameter.lower, variable.parameter.upper, variable.parameter.initial}) {
                bound_value(value,
                            assignment,
                            integer_variable(variable) && integral(assignment.scale) &&
                                integral(assignment.offset));
            }
        }
    }
    OptimizationTaskResult<Result> result;
    result.dependency_revisions = revisions(analysis);
    result.search = minimize_bounded(
        native,
        [&](const auto &point) {
            auto bound = analysis_task_detail::bind(initial, variables, point);
            ++result.analysis_evaluations;
            auto evaluated = analysis.evaluate(bound);
            auto measured = measure(evaluated, objectives);
            if (!result.best || measured.first < result.best->value) {
                BestAnalysis<Result> best{point,
                                          std::move(bound),
                                          std::make_shared<const Result>(std::move(evaluated)),
                                          std::move(measured.second),
                                          measured.first};
                result.best = std::move(best);
            }
            return measured.first;
        },
        options,
        cancelled);
    if (revisions(analysis) != result.dependency_revisions) {
        throw std::runtime_error("analysis dependency changed during optimization");
    }
    if (result.search.has_finite_point &&
        (!result.best || result.search.parameters != result.best->variable_values ||
         result.search.value != result.best->value)) {
        throw std::logic_error("optimizer and saved analysis disagree");
    }
    return result;
}

template <class Result>
inline MonteCarloTaskResult run_monte_carlo_task(ParameterizedAnalysis<Result> analysis,
                                                 std::vector<MonteCarloVariable> variables,
                                                 std::vector<AnalysisMeasurement<Result>> metrics,
                                                 MonteCarloOptions options = {},
                                                 const std::function<bool()> &cancelled = {}) {
    using namespace analysis_task_detail;
    const auto initial = validate(analysis);
    validate_bindings(variables, initial.size());
    std::set<std::string> names;
    for (const auto &metric : metrics) {
        validate(metric);
        if (!names.insert(metric.name).second) {
            throw std::invalid_argument("duplicate Monte Carlo metric");
        }
    }
    std::vector<MonteCarloParameter> native;
    for (const auto &variable : variables) {
        native.push_back(variable.parameter);
    }
    monte_carlo_detail::validate(native, metrics.size(), options);
    for (const auto &variable : variables) {
        const auto kind = variable.parameter.distribution;
        if (kind == MonteCarloDistribution::Normal || kind == MonteCarloDistribution::LogNormal) {
            continue;
        }
        for (const auto &assignment : variable.assignments) {
            for (double value :
                 {variable.parameter.first,
                  kind == MonteCarloDistribution::Fixed ? variable.parameter.first
                                                        : variable.parameter.second}) {
                bound_value(value,
                            assignment,
                            integer_variable(variable) && integral(assignment.scale) &&
                                integral(assignment.offset));
            }
        }
    }
    MonteCarloTaskResult result;
    for (const auto &metric : metrics) {
        result.metric_names.push_back(metric.name);
    }
    result.dependency_revisions = revisions(analysis);
    result.sampling = run_monte_carlo(
        native,
        metrics.size(),
        [&](const auto &point) {
            const auto bound = analysis_task_detail::bind(initial, variables, point);
            ++result.analysis_evaluations;
            auto evaluated = analysis.evaluate(bound);
            std::vector<double> measured;
            for (const auto &metric : metrics) {
                measured.push_back(project(metric.read(evaluated), metric.projection));
            }
            return measured;
        },
        options,
        cancelled);
    if (revisions(analysis) != result.dependency_revisions) {
        throw std::runtime_error("analysis dependency changed during Monte Carlo");
    }
    return result;
}
} // namespace rfmodel
