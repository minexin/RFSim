#include "rfmodel/analysis/monte_carlo.hpp"
#include "rfmodel/linear/network.hpp"
#include "rfmodel/models/rlc_model.hpp"
#include <iomanip>
#include <iostream>

static void write_joint(const rfmodel::MonteCarloJointStatistics &joint) {
    std::cout << "{\"dimension\":" << joint.dimension << ",\"count\":" << joint.count;
    for (bool correlation : {false, true}) {
        std::cout << (correlation ? ",\"sample_correlation\":[" : ",\"sample_covariance\":[");
        const auto &values = correlation ? joint.sample_correlation : joint.sample_covariance;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) {
                std::cout << ',';
            }
            if (values[i]) {
                std::cout << *values[i];
            } else {
                std::cout << "null";
            }
        }
        std::cout << ']';
    }
    std::cout << '}';
}

int main() {
    try {
        using namespace rfmodel;
        MonteCarloOptions options;
        options.samples = 4096;
        options.seed = 20261012;
        options.latent_correlation = {1., .8, .8, 1.};
        options.joint_statistics = true;
        options.criteria = {{{.25}, {}}, {{}, {.25}}, {}};
        const auto result = run_monte_carlo(
            {{MonteCarloDistribution::Normal, 50., 2.}, {MonteCarloDistribution::Normal, 50., 2.}},
            3,
            [](const auto &x) {
                LinearNetwork network;
                IdealRLCModel first(
                    "R1", IdealElement::Resistor, LumpedConnection::SeriesImpedance, x[0]);
                IdealRLCModel second(
                    "R2", IdealElement::Resistor, LumpedConnection::SeriesImpedance, x[1]);
                network.add(first.s_parameters(1e9));
                network.add(second.s_parameters(1e9));
                network.connect(1, 2);
                const auto s = network.external_s({0, 3});
                // Infer total series resistance from the actual network transmission.
                return std::vector<double>{
                    std::norm(s(1, 0)), std::norm(s(0, 0)), (100. / s(1, 0) - 100.).real()};
            },
            options);
        if (result.accepted_samples != 4096 || result.random_words != 16384 ||
            result.latent_rank != 2) {
            throw std::runtime_error("unexpected correlated sampling outcome");
        }
        for (const auto &e : result.history) {
            const double total = e.parameters[0] + e.parameters[1];
            if (std::abs(e.values[0] - std::pow(100. / (100. + total), 2)) > 1e-12 ||
                std::abs(e.values[1] - std::pow(total / (100. + total), 2)) > 1e-12 ||
                std::abs(e.values[2] - total) > 1e-10 || e.passed != (total <= 100.)) {
                throw std::runtime_error("independent correlated resistor circuit check failed");
            }
        }
        const double covariance_se = std::sqrt((16. + 3.2 * 3.2) / 4095.);
        if (std::abs(result.statistics[2].mean - 100.) > 6. * std::sqrt(14.4 / 4096.) ||
            std::abs(*result.statistics[2].sample_variance - 14.4) >
                6. * 14.4 * std::sqrt(2. / 4095.) ||
            std::abs(*result.parameter_joint.sample_covariance[1] - 3.2) > 6. * covariance_se ||
            std::abs(*result.passed_fraction - .5) > 6. * std::sqrt(.25 / 4096.)) {
            throw std::runtime_error("correlated resistor analytic statistics check failed");
        }
        std::cout << std::setprecision(17)
                  << "{\n\"format\":\"rfmodel.correlated-resistors-example\",\"version\":1,\n"
                  << "\"algorithm\":\"" << result.algorithm << "\",\"seed\":" << result.seed
                  << ",\"samples\":" << options.samples
                  << ",\"random_words\":" << result.random_words
                  << ",\"accepted_samples\":" << result.accepted_samples
                  << ",\"latent_rank\":" << result.latent_rank
                  << ",\"latent_correlation\":[1,0.8,0.8,1],\"effective_latent_correlation\":[";
        for (std::size_t i = 0; i < 4; ++i) {
            if (i) {
                std::cout << ',';
            }
            std::cout << result.effective_latent_correlation[i];
        }
        std::cout
            << "],\n\"frequency_hz\":1000000000,\"reference_ohms\":50,"
            << "\"parameter_order\":[\"R1_ohms\",\"R2_ohms\"],"
            << "\"distributions\":[{\"kind\":\"normal\",\"mean\":50,\"standard_deviation\":2},{"
               "\"kind\":\"normal\",\"mean\":50,\"standard_deviation\":2}],"
            << "\"metric_order\":[\"transmitted_power_fraction\",\"reflected_power_fraction\","
               "\"inferred_total_resistance_ohms\"],"
            << "\"criteria\":[{\"lower\":0.25},{\"upper\":0.25},{}],\"invalid_sample\":\"stop\","
               "\"joint_statistics\":true,\"record_history\":true,"
            << "\"quantile_levels\":[0.05,0.5,0.95],\"passed_samples\":" << result.passed_samples
            << ",\"passed_fraction\":" << *result.passed_fraction
            << ",\"analytic_total_resistance_mean\":100,\"analytic_total_resistance_variance\":14."
               "4,\"analytic_parameter_covariance\":3.2,\"analytic_pass_probability\":0.5,"
            << "\"checks\":{\"mean_covariance_variance_pass_SE_multiplier\":6,\"power_abs_"
               "tolerance\":1e-12,\"resistance_abs_tolerance\":1e-10},\n\"parameter_joint\":";
        write_joint(result.parameter_joint);
        std::cout << ",\n\"metric_joint\":";
        write_joint(result.metric_joint);
        std::cout << ",\n\"statistics\":[";
        for (std::size_t j = 0; j < 3; ++j) {
            if (j) {
                std::cout << ',';
            }
            const auto &s = result.statistics[j];
            std::cout << "{\"count\":" << s.count << ",\"mean\":" << s.mean
                      << ",\"minimum\":" << s.minimum << ",\"maximum\":" << s.maximum
                      << ",\"sample_variance\":" << *s.sample_variance << ",\"quantiles\":[";
            for (std::size_t k = 0; k < s.quantiles.size(); ++k) {
                if (k) {
                    std::cout << ',';
                }
                std::cout << s.quantiles[k];
            }
            std::cout << "]}";
        }
        std::cout << "],\n\"history\":[\n";
        for (std::size_t i = 0; i < result.history.size(); ++i) {
            if (i) {
                std::cout << ",\n";
            }
            const auto &e = result.history[i];
            std::cout << "{\"index\":" << i << ",\"parameters\":[" << e.parameters[0] << ','
                      << e.parameters[1] << "],\"values\":[" << e.values[0] << ',' << e.values[1]
                      << ',' << e.values[2] << "],\"passed\":" << (e.passed ? "true" : "false")
                      << '}';
        }
        std::cout << "\n]}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
