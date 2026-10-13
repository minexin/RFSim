#include "rfmodel/analysis_task.hpp"
#include "rfmodel/network.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
SMatrix resistors(const std::vector<double> &values) {
    LinearNetwork network;
    for (std::size_t i = 0; i < values.size(); ++i) {
        IdealRLCModel resistor(
            "R", IdealElement::Resistor, LumpedConnection::SeriesImpedance, values[i]);
        network.add(resistor.s_parameters(1e9));
        if (i) {
            network.connect(2 * i - 1, 2 * i);
        }
    }
    return network.external_s({0, values.size() * 2 - 1});
}

void optimize_physical_task() {
    std::size_t calls = 0;
    ParameterizedAnalysis<SMatrix> analysis{
        {{"R1", 25., "ohm"}, {"R2", 55., "ohm"}},
        [&](const auto &point) {
            ++calls;
            require(point[1] == 2. * point[0] + 5., "shared binding was sampled independently");
            auto result = resistors(point);
            near(result(1, 0), 100. / (100. + point[0] + point[1]));
            return result;
        },
        [] {
            return std::vector<std::string>{"immutable-circuit-v1"};
        }};
    std::vector<OptimizationVariable> variables{{"R", {5., 100., 25.}, {{0}, {1, 2., 5.}}}};
    std::vector<AnalysisObjective<SMatrix>> objectives{{{"transmission",
                                                         [](const auto &s) {
                                                             return s(1, 0);
                                                         },
                                                         MeasurementProjection::real},
                                                        ObjectiveGoal::target,
                                                        1.,
                                                        1.,
                                                        .5}};
    OptimizationOptions options;
    options.target = 1e-20;
    options.minimum_step = 1e-10;
    auto result = run_optimization_task(analysis, variables, objectives, options);
    require(result.search.has_finite_point && result.best &&
                result.search.stop == OptimizationStop::TargetReached,
            "physical optimization failed");
    require(calls == result.analysis_evaluations && calls == result.search.evaluations,
            "saved best analysis caused an extra evaluation");
    require(std::abs(result.best->variable_values[0] - 95. / 3.) < 1e-6, "wrong resistor optimum");
    const auto saved_calls = calls;
    near((*result.best->analysis)(1, 0), 100. / (105. + 3. * result.best->variable_values[0]));
    require(calls == saved_calls && result.best->bound_parameters.size() == 2,
            "reading a snapshot recomputed the RF model");
    require(analysis.parameters[0].value == 25. && variables[0].assignments[1].scale == 2.,
            "task mutated input specification");
    auto cancelled = run_optimization_task(analysis, variables, objectives, options, [] {
        return true;
    });
    require(!cancelled.best && cancelled.analysis_evaluations == 0 &&
                cancelled.search.stop == OptimizationStop::Cancelled,
            "pre-cancel evaluated a model");
    auto overlap = variables;
    overlap[0].assignments.push_back({0});
    rejects<std::invalid_argument>([&] {
        run_optimization_task(analysis, overlap, objectives, options);
    });
}

void correlated_tolerance_task() {
    std::size_t calls = 0;
    ParameterizedAnalysis<SMatrix> analysis{
        {{"R1", 20., "ohm"}, {"R2", 45., "ohm"}, {"R3", 30., "ohm"}},
        [&](const auto &point) {
            ++calls;
            return resistors(point);
        },
        {}};
    std::vector<MonteCarloVariable> variables{
        {"shared", {MonteCarloDistribution::Uniform, 10., 30.}, {{0}, {1, 2., 5.}}},
        {"separate", {MonteCarloDistribution::Uniform, 20., 40.}, {{2}}}};
    std::vector<AnalysisMeasurement<SMatrix>> metrics{{"transmitted_power",
                                                       [](const auto &s) {
                                                           return s(1, 0);
                                                       },
                                                       MeasurementProjection::power},
                                                      {"reflected",
                                                       [](const auto &s) {
                                                           return s(0, 0);
                                                       },
                                                       MeasurementProjection::real}};
    MonteCarloOptions options;
    options.samples = 2048;
    options.seed = 431;
    options.latent_correlation = {1., .6, .6, 1.};
    options.joint_statistics = true;
    options.criteria = {{{.2}, {.35}}, {{}, {.6}}};
    const auto result = run_monte_carlo_task(analysis, variables, metrics, options);
    require(result.sampling.accepted_samples == options.samples &&
                result.analysis_evaluations == options.samples && calls == options.samples &&
                result.sampling.latent_rank == 2,
            "Monte Carlo task counts/rank");
    std::size_t passed = 0;
    double sum = 0.;
    for (const auto &row : result.sampling.history) {
        const double resistance = 3. * row.parameters[0] + 5. + row.parameters[1];
        const double amplitude = 100. / (100. + resistance);
        const double power = amplitude * amplitude;
        near(row.values[0], power);
        near(row.values[1], resistance / (100. + resistance));
        const bool valid = power >= .2 && power <= .35 && row.values[1] <= .6;
        require(row.passed == valid, "joint pass criteria differ");
        passed += valid;
        sum += power;
    }
    near(result.sampling.statistics[0].mean, sum / options.samples);
    require(result.sampling.passed_samples == passed &&
                result.sampling.metric_joint.count == options.samples,
            "statistics did not use the accepted sample set");
    const auto replay = run_monte_carlo_task(analysis, variables, metrics, options);
    for (std::size_t i = 0; i < options.samples; ++i) {
        require(replay.sampling.history[i].parameters == result.sampling.history[i].parameters &&
                    replay.sampling.history[i].values == result.sampling.history[i].values,
                "task replay differs");
    }
}

void integer_mapping_and_projection() {
    const double limit = 9007199254740991.;
    ParameterizedAnalysis<Complex> analysis{{{"n", 0., ""}},
                                            [](const auto &v) {
                                                return Complex{v[0], 4.};
                                            },
                                            {}};
    std::vector<OptimizationVariable> variables{
        {"integer", {limit, limit, limit, OptimizationParameterKind::Integer}, {{0, 2., -limit}}}};
    std::vector<AnalysisObjective<Complex>> goals{{{"real",
                                                    [](Complex value) {
                                                        return value;
                                                    },
                                                    MeasurementProjection::real}}};
    auto result = run_optimization_task(analysis, variables, goals);
    require(result.best->bound_parameters[0] == limit,
            "integer product rounded before offset cancellation");
    variables[0].assignments[0].offset = 0.;
    rejects<std::overflow_error>([&] {
        run_optimization_task(analysis, variables, goals);
    });
    analysis.parameters[0].value = 3.;
    variables = {{"fixed", {3., 3., 3.}, {{0}}}};
    const std::vector<std::pair<MeasurementProjection, double>> expected{
        {MeasurementProjection::real, 3.},
        {MeasurementProjection::imaginary, 4.},
        {MeasurementProjection::magnitude, 5.},
        {MeasurementProjection::power, 25.},
        {MeasurementProjection::magnitude_db, 20. * std::log10(5.)},
        {MeasurementProjection::phase_radians, std::atan2(4., 3.)}};
    for (const auto &value : expected) {
        goals[0].measurement.projection = value.first;
        near(run_optimization_task(analysis, variables, goals).search.value, value.second);
    }
    goals[0].measurement.projection = MeasurementProjection::value;
    require(run_optimization_task(analysis, variables, goals).search.stop ==
                OptimizationStop::ObjectiveFailure,
            "complex scalar was silently converted");
}

void failure_snapshot_and_dependencies() {
    std::string revision = "before";
    ParameterizedAnalysis<double> analysis{{{"x", 1., ""}},
                                           [&](const auto &v) {
                                               revision = "after";
                                               return v[0];
                                           },
                                           [&] {
                                               return std::vector<std::string>{revision};
                                           }};
    std::vector<OptimizationVariable> variables{{"x", {0., 2., 1.}, {{0}}}};
    std::vector<AnalysisObjective<double>> goals{{{"value", [](double value) {
                                                       return value;
                                                   }}}};
    rejects<std::runtime_error>([&] {
        run_optimization_task(analysis, variables, goals);
    });
    analysis.dependency_revisions = {};
    std::size_t calls = 0;
    analysis.evaluate = [&](const auto &v) {
        ++calls;
        if (v[0] == 0.) {
            throw std::runtime_error("host solver failed");
        }
        return v[0];
    };
    OptimizationOptions options;
    options.max_evaluations = 8;
    options.invalid_evaluation = OptimizationFailurePolicy::Reject;
    const auto result = run_optimization_task(analysis, variables, goals, options);
    require(calls == result.search.evaluations && result.best &&
                result.search.rejected_evaluations > 0,
            "rejected host evaluations were not counted");
    require(*result.best->analysis == result.search.value,
            "best snapshot differs after rejected evaluation");
}
} // namespace

int main() {
    try {
        optimize_physical_task();
        correlated_tolerance_task();
        integer_mapping_and_projection();
        failure_snapshot_and_dependencies();
        std::cout << "Typed parameter tasks: independent circuits, bindings, projections, "
                     "statistics and saved evaluations passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
