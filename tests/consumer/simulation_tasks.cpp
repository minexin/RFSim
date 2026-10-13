#include <rfmodel/analysis_task.hpp>
#include <rfmodel/coherent_system.hpp>
#include <rfmodel/conversion_simulation.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
rfmodel::CoherentSystem::Plan make_simulation_plan();

int main() {
    try {
        using namespace rfmodel;
        const auto result = make_simulation_plan().execute();
        if (std::abs(result.stream("if").reduction.total_power_w - .0008) > 1e-14) {
            throw std::runtime_error("installed graph power");
        }
        ConversionGrid grid(1e8, {{{0}, {{0, 1}}, {}}}, {});
        ConversionSeedState seed;
        seed.source = {.02};
        const auto state = grid.expand(0, seed);
        FrequencyConversionModel model(1e8, grid.plan().channels[0], {1, {.5}}, {1, {0.}});
        ConversionSimulationRequest request;
        request.devices.push_back(
            {model, state.source, state.reflection, state.source_noise, state.intrinsic_noise});
        const auto conversion = analyze_conversion_simulation(request);
        if (std::abs(conversion.waves.outgoing[0] - Complex{.01}) > 1e-14) {
            throw std::runtime_error("installed conversion result");
        }
        ParameterizedAnalysis<ConversionSimulationResult> analysis{
            {{"input_amplitude", .02, "sqrt(W)"}},
            [request](const auto &parameters) mutable {
                request.devices[0].source[0] = parameters[0];
                return analyze_conversion_simulation(request);
            },
            {}};
        std::vector<OptimizationVariable> variables{{"amplitude", {.001, .1, .02}, {{0}}}};
        std::vector<AnalysisObjective<ConversionSimulationResult>> objectives{
            {{"output_power",
              [](const auto &r) {
                  return r.waves.outgoing[0];
              },
              MeasurementProjection::power},
             ObjectiveGoal::target,
             1.,
             1.,
             .0004}};
        OptimizationOptions options;
        options.target = 1e-22;
        options.minimum_step = 1e-10;
        const auto task = run_optimization_task(analysis, variables, objectives, options);
        if (!task.best || std::abs(task.best->variable_values[0] - .04) > 1e-6) {
            throw std::runtime_error("installed conversion optimization");
        }
        std::vector<MonteCarloVariable> tolerance{
            {"amplitude", {MonteCarloDistribution::Uniform, .019, .021}, {{0}}}};
        std::vector<AnalysisMeasurement<ConversionSimulationResult>> metrics{
            objectives[0].measurement};
        MonteCarloOptions sampling;
        sampling.samples = 32;
        const auto samples = run_monte_carlo_task(analysis, tolerance, metrics, sampling);
        if (samples.sampling.accepted_samples != 32) {
            throw std::runtime_error("installed sampling task");
        }
        std::cout << "Installed C++ simulation graph, frequency grid, conversion, optimization and "
                     "tolerance tasks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
