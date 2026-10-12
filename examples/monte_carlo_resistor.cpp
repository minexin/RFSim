#include "rfmodel/monte_carlo.hpp"
#include "rfmodel/network.hpp"
#include "rfmodel/rlc_model.hpp"
#include <iomanip>
#include <iostream>

int main() {
    try {
        using namespace rfmodel;
        const std::vector<MonteCarloParameter> parameters{
            {MonteCarloDistribution::Uniform, 45., 55.}, {MonteCarloDistribution::Fixed, 50., 0.}};
        MonteCarloOptions options;
        options.samples = 4096;
        options.seed = 20261012;
        options.criteria = {{{.25}, {}}, {{}, {.25}}};
        const auto result = run_monte_carlo(
            parameters,
            2,
            [](const auto &x) {
                IdealRLCModel r1(
                    "R1", IdealElement::Resistor, LumpedConnection::SeriesImpedance, x[0]);
                IdealRLCModel r2(
                    "R2", IdealElement::Resistor, LumpedConnection::SeriesImpedance, x[1]);
                LinearNetwork network;
                network.add(r1.s_parameters(1e9));
                network.add(r2.s_parameters(1e9));
                network.connect(1, 2);
                const auto s = network.external_s({0, 3});
                return std::vector<double>{std::norm(s(1, 0)), std::norm(s(0, 0))};
            },
            options);
        if (result.stop != MonteCarloStop::Completed ||
            result.accepted_samples != options.samples || result.random_words != options.samples) {
            throw std::runtime_error("unexpected Monte Carlo sampling outcome");
        }
        double sums[2]{}, sum_squares[2]{};
        for (const auto &e : result.history) {
            const double total = e.parameters[0] + e.parameters[1];
            const double expected[2]{std::pow(100. / (100. + total), 2),
                                     std::pow(total / (100. + total), 2)};
            for (std::size_t j = 0; j < 2; ++j) {
                if (std::abs(e.values[j] - expected[j]) > 1e-12) {
                    throw std::runtime_error("per-sample independent circuit reference failed");
                }
                sums[j] += expected[j];
            }
            if (e.passed != (e.parameters[0] <= 50.)) {
                throw std::runtime_error("pass criteria circuit reference failed");
            }
        }
        for (const auto &e : result.history) {
            for (std::size_t j = 0; j < 2; ++j) {
                sum_squares[j] +=
                    std::pow(e.values[j] - sums[j] / static_cast<double>(options.samples), 2);
            }
        }
        for (std::size_t j = 0; j < 2; ++j) {
            if (std::abs(result.statistics[j].mean -
                         sums[j] / static_cast<double>(options.samples)) > 1e-12 ||
                std::abs(*result.statistics[j].sample_variance -
                         sum_squares[j] / static_cast<double>(options.samples - 1)) > 1e-12) {
                throw std::runtime_error("independent two-pass statistics failed");
            }
        }
        const double population_mean = 1000. * (1. / 195. - 1. / 205.);
        const double population_variance =
            1e7 / 3. * (1. / std::pow(195., 3) - 1. / std::pow(205., 3)) -
            population_mean * population_mean;
        const double population_reflection = 1. - 20. * std::log(205. / 195.) + population_mean;
        if (std::abs(result.statistics[0].mean - population_mean) >
                6. * std::sqrt(population_variance / static_cast<double>(options.samples)) ||
            std::abs(*result.passed_fraction - .5) >
                6. * std::sqrt(.25 / static_cast<double>(options.samples))) {
            throw std::runtime_error("analytic mean or pass probability consistency failed");
        }
        // DKW probability-coordinate envelope, with alpha fixed before observing results.
        const double epsilon =
            std::sqrt(std::log(2. / 1e-6) / (2. * static_cast<double>(options.samples)));
        auto quantile = [](std::size_t j, double p) {
            return j == 0 ? std::pow(100. / (205. - 10. * p), 2)
                          : std::pow((95. + 10. * p) / (195. + 10. * p), 2);
        };
        for (std::size_t j = 0; j < 2; ++j) {
            for (std::size_t k = 0; k < options.quantile_levels.size(); ++k) {
                const double p = options.quantile_levels[k],
                             observed = result.statistics[j].quantiles[k];
                if (observed < quantile(j,
                                        std::max(0.,
                                                 p - epsilon -
                                                     1. / static_cast<double>(options.samples))) ||
                    observed > quantile(j,
                                        std::min(1.,
                                                 p + epsilon +
                                                     1. / static_cast<double>(options.samples)))) {
                    throw std::runtime_error("analytic quantile consistency failed");
                }
            }
        }
        std::cout
            << std::setprecision(17)
            << "{\n  \"format\":\"rfmodel.monte-carlo-example\",\"version\":1,\n"
            << "  \"algorithm\":\"" << monte_carlo_random_algorithm << "\",\n"
            << "  \"frequency_hz\":1000000000,\"reference_ohms\":50,\n"
            << "  \"parameter_order\":[\"R1_ohms\",\"R2_ohms\"],\n"
            << "  "
               "\"distributions\":[{\"kind\":\"uniform\",\"lower\":45,\"upper\":55},{\"kind\":"
               "\"fixed\",\"value\":50}],\n"
            << "  \"metric_order\":[\"transmitted_power_fraction\",\"reflected_power_fraction\"],\n"
            << "  "
               "\"criteria\":[{\"lower\":0.25},{\"upper\":0.25}],\"quantile_levels\":[0.05,0.5,0."
               "95],\n"
            << "  \"invalid_sample\":\"stop\",\"record_history\":true,\n"
            << "  \"seed\":" << result.seed << ",\"requested_samples\":" << result.requested_samples
            << ",\"samples_attempted\":" << result.samples_attempted
            << ",\"evaluations\":" << result.evaluations
            << ",\"accepted_samples\":" << result.accepted_samples
            << ",\"rejected_samples\":" << result.rejected_samples
            << ",\"random_words\":" << result.random_words << ",\n"
            << "  \"stop\":\"" << monte_carlo_stop_name(result.stop)
            << "\",\"passed_samples\":" << result.passed_samples
            << ",\"passed_fraction\":" << *result.passed_fraction
            << ",\"valid_pass_fraction\":" << *result.valid_pass_fraction << ",\n"
            << "  \"analytic_population_means\":[" << population_mean << ","
            << population_reflection << "],\n"
            << "  \"analytic_gain_population_variance\":" << population_variance
            << ",\"analytic_pass_probability\":0.5,\n"
            << "  "
               "\"checks\":{\"sample_circuit_abs_tolerance\":1e-12,\"mean_and_pass_SE_multiplier\":"
               "6,\"quantile_DKW_alpha\":1e-6,\"quantile_probability_slack\":"
            << 1. / static_cast<double>(options.samples) << "},\n"
            << "  \"statistics\":[";
        for (std::size_t j = 0; j < 2; ++j) {
            const auto &s = result.statistics[j];
            if (j) {
                std::cout << ",";
            }
            std::cout << "{\"count\":" << s.count << ",\"mean\":" << s.mean
                      << ",\"minimum\":" << s.minimum << ",\"maximum\":" << s.maximum
                      << ",\"sample_variance\":" << *s.sample_variance
                      << ",\"sample_standard_deviation\":" << *s.sample_standard_deviation
                      << ",\"standard_error\":" << *s.standard_error << ",\"quantiles\":[";
            for (std::size_t k = 0; k < s.quantiles.size(); ++k) {
                if (k) {
                    std::cout << ",";
                }
                std::cout << s.quantiles[k];
            }
            std::cout << "]}";
        }
        std::cout << "],\n  \"history\":[\n";
        for (std::size_t i = 0; i < result.history.size(); ++i) {
            const auto &e = result.history[i];
            if (i) {
                std::cout << ",\n";
            }
            std::cout << "    {\"index\":" << e.index << ",\"parameters\":[" << e.parameters[0]
                      << "," << e.parameters[1] << "],\"values\":[" << e.values[0] << ","
                      << e.values[1]
                      << "],\"status\":\"accepted\",\"passed\":" << (e.passed ? "true" : "false")
                      << "}";
        }
        std::cout << "\n  ]\n}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
