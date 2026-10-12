#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rfmodel {
enum class MonteCarloDistribution {
    Fixed,
    Uniform,
    LogUniform,
    Normal,
    LogNormal,
    DiscreteUniform
};
enum class MonteCarloFailurePolicy {
    Stop,
    Reject
};
enum class MonteCarloStop {
    Completed,
    SamplingFailure,
    EvaluationFailure,
    Cancelled
};
enum class MonteCarloSampleStatus {
    Accepted,
    SamplingFailure,
    NonFiniteMeasurement,
    InvalidMeasurementCount,
    Exception
};

struct MonteCarloParameter {
    MonteCarloDistribution distribution{MonteCarloDistribution::Fixed};
    double first{}, second{};
};

struct MonteCarloLimits {
    std::optional<double> lower, upper;
};

struct MonteCarloOptions {
    std::size_t samples{1000};
    std::uint64_t seed{};
    MonteCarloFailurePolicy invalid_sample{MonteCarloFailurePolicy::Stop};
    bool record_history{true};
    std::vector<double> quantile_levels{.05, .5, .95};
    std::vector<MonteCarloLimits> criteria;
};

struct MonteCarloStatistics {
    std::size_t count{};
    double mean{std::numeric_limits<double>::quiet_NaN()};
    double minimum{std::numeric_limits<double>::quiet_NaN()};
    double maximum{std::numeric_limits<double>::quiet_NaN()};
    std::optional<double> sample_variance, sample_standard_deviation, standard_error;
    std::vector<double> quantiles;
};

struct MonteCarloSample {
    std::size_t index{};
    std::vector<double> parameters, values;
    MonteCarloSampleStatus status{MonteCarloSampleStatus::Accepted};
    std::string error;
    bool passed{};
};

struct MonteCarloResult {
    std::uint64_t seed{}, random_words{};
    std::size_t requested_samples{}, samples_attempted{}, evaluations{}, accepted_samples{},
        rejected_samples{};
    MonteCarloStop stop{MonteCarloStop::Completed};
    bool has_statistics{}, criteria_enabled{};
    std::size_t passed_samples{};
    std::optional<double> passed_fraction, valid_pass_fraction;
    std::vector<double> quantile_levels;
    std::vector<MonteCarloStatistics> statistics;
    std::vector<double> last_failed_parameters;
    std::string last_error;
    std::vector<MonteCarloSample> history;
};

inline constexpr const char *monte_carlo_random_algorithm = "mt19937_64-box-muller52-v1";

inline const char *monte_carlo_stop_name(MonteCarloStop stop) {
    switch (stop) {
    case MonteCarloStop::Completed:
        return "completed";
    case MonteCarloStop::SamplingFailure:
        return "sampling_failure";
    case MonteCarloStop::EvaluationFailure:
        return "evaluation_failure";
    case MonteCarloStop::Cancelled:
        return "cancelled";
    }
    throw std::invalid_argument("unknown Monte Carlo stop");
}

namespace monte_carlo_detail {
inline double interpolate(double a, double b, double t) {
    if (t == 0.) {
        return a;
    }
    if (t == 1.) {
        return b;
    }
    return std::clamp((a < 0.) != (b < 0.) ? (1. - t) * a + t * b : a + t * (b - a), a, b);
}

inline void validate(const std::vector<MonteCarloParameter> &parameters,
                     std::size_t metrics,
                     const MonteCarloOptions &options) {
    if (parameters.empty() || parameters.size() > 64 || metrics == 0 || metrics > 64 ||
        options.samples == 0 || options.samples > 100000 || options.quantile_levels.size() > 256 ||
        (options.invalid_sample != MonteCarloFailurePolicy::Stop &&
         options.invalid_sample != MonteCarloFailurePolicy::Reject)) {
        throw std::invalid_argument("invalid Monte Carlo dimensions, sample budget or policy");
    }
    double previous = -1.;
    for (double q : options.quantile_levels) {
        if (!std::isfinite(q) || q < 0. || q > 1. || q <= previous) {
            throw std::invalid_argument("quantile levels must be finite, increasing and in [0,1]");
        }
        previous = q;
    }
    if (!options.criteria.empty()) {
        if (options.criteria.size() != metrics) {
            throw std::invalid_argument("criteria dimension mismatch");
        }
        bool any = false;
        for (const auto &c : options.criteria) {
            if ((c.lower && !std::isfinite(*c.lower)) || (c.upper && !std::isfinite(*c.upper)) ||
                (c.lower && c.upper && *c.lower > *c.upper)) {
                throw std::invalid_argument("invalid Monte Carlo limits");
            }
            any = any || c.lower.has_value() || c.upper.has_value();
        }
        if (!any) {
            throw std::invalid_argument("criteria require at least one limit");
        }
    }
    for (const auto &p : parameters) {
        if (!std::isfinite(p.first) || !std::isfinite(p.second)) {
            throw std::invalid_argument("nonfinite distribution parameter");
        }
        switch (p.distribution) {
        case MonteCarloDistribution::Fixed:
            if (p.second != 0.) {
                throw std::invalid_argument("fixed distribution second parameter must be zero");
            }
            break;
        case MonteCarloDistribution::Uniform:
        case MonteCarloDistribution::LogUniform:
        case MonteCarloDistribution::DiscreteUniform:
            if (p.first > p.second) {
                throw std::invalid_argument("reversed distribution bounds");
            }
            if (p.distribution == MonteCarloDistribution::LogUniform && p.first <= 0.) {
                throw std::invalid_argument("log uniform requires positive bounds");
            }
            if (p.distribution == MonteCarloDistribution::DiscreteUniform &&
                (std::abs(p.first) > 9007199254740991. || std::abs(p.second) > 9007199254740991. ||
                 std::trunc(p.first) != p.first || std::trunc(p.second) != p.second)) {
                throw std::invalid_argument("discrete uniform requires exact safe integer bounds");
            }
            break;
        case MonteCarloDistribution::Normal:
        case MonteCarloDistribution::LogNormal:
            if (p.second < 0.) {
                throw std::invalid_argument("negative distribution standard deviation");
            }
            if (p.distribution == MonteCarloDistribution::LogNormal && p.second == 0. &&
                (!std::isfinite(std::exp(p.first)) || std::exp(p.first) == 0.)) {
                throw std::invalid_argument("constant log normal value is not representable");
            }
            break;
        default:
            throw std::invalid_argument("unknown Monte Carlo distribution");
        }
    }
}

struct Random {
    std::mt19937_64 engine;
    std::uint64_t words{};

    explicit Random(std::uint64_t seed) : engine(seed) {
    }

    std::uint64_t word() {
        ++words;
        return engine();
    }

    double unit() {
        return static_cast<double>(word() >> 11) * 0x1p-53;
    }

    double open_unit() {
        return (static_cast<double>(word() >> 12) + .5) * 0x1p-52;
    }

    double normal() {
        const double u = open_unit(), v = open_unit();
        return std::sqrt(-2. * std::log(u)) * std::cos(2. * std::acos(-1.) * v);
    }

    double sample(const MonteCarloParameter &p) {
        switch (p.distribution) {
        case MonteCarloDistribution::Fixed:
            return p.first;
        case MonteCarloDistribution::Uniform:
            return p.first == p.second ? p.first : interpolate(p.first, p.second, unit());
        case MonteCarloDistribution::LogUniform:
            return p.first == p.second
                       ? p.first
                       : std::clamp(
                             std::exp(interpolate(std::log(p.first), std::log(p.second), unit())),
                             p.first,
                             p.second);
        case MonteCarloDistribution::Normal:
        case MonteCarloDistribution::LogNormal: {
            const double x = p.second == 0. ? p.first : std::fma(p.second, normal(), p.first);
            return p.distribution == MonteCarloDistribution::Normal ? x : std::exp(x);
        }
        case MonteCarloDistribution::DiscreteUniform: {
            if (p.first == p.second) {
                return p.first;
            }
            const auto low = static_cast<std::int64_t>(p.first);
            const auto width =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(p.second) - low) + 1;
            const auto threshold = (std::uint64_t{0} - width) % width;
            std::uint64_t raw;
            do {
                raw = word();
            } while (raw < threshold);
            return static_cast<double>(low + static_cast<std::int64_t>(raw % width));
        }
        }
        throw std::invalid_argument("unknown Monte Carlo distribution");
    }
};

struct Sum {
    double value{}, correction{};

    void add(double x) {
        const double adjusted = x - correction, next = value + adjusted;
        correction = (next - value) - adjusted;
        value = next;
    }
};

inline std::optional<double> representable(double value) {
    return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}

inline MonteCarloStatistics summarize(std::vector<double> &values,
                                      const std::vector<double> &levels) {
    MonteCarloStatistics result;
    result.count = values.size();
    if (values.empty()) {
        return result;
    }
    const auto bounds = std::minmax_element(values.begin(), values.end());
    result.minimum = *bounds.first;
    result.maximum = *bounds.second;
    const double center = interpolate(result.minimum, result.maximum, .5);
    const double scale =
        std::max(std::abs(result.minimum - center), std::abs(result.maximum - center));
    Sum sum;
    if (scale != 0.) {
        for (double x : values) {
            sum.add((x - center) / scale);
        }
    }
    const double mean_normalized = sum.value / static_cast<double>(values.size());
    result.mean =
        std::clamp(std::fma(scale, mean_normalized, center), result.minimum, result.maximum);
    if (values.size() >= 2) {
        Sum squares;
        if (scale != 0.) {
            for (double x : values) {
                const double d = (x - center) / scale - mean_normalized;
                squares.add(d * d);
            }
        }
        const double normalized_variance = squares.value / static_cast<double>(values.size() - 1);
        const double sd = scale * std::sqrt(normalized_variance);
        result.sample_standard_deviation = representable(sd);
        result.sample_variance = representable(sd * sd);
        result.standard_error = representable(
            scale * std::sqrt(normalized_variance / static_cast<double>(values.size())));
    }
    if (!levels.empty()) {
        std::sort(values.begin(), values.end());
        for (double q : levels) {
            const double h = static_cast<double>(values.size() - 1) * q;
            const auto i = static_cast<std::size_t>(h);
            result.quantiles.push_back(interpolate(
                values[i], values[std::min(i + 1, values.size() - 1)], h - static_cast<double>(i)));
        }
    }
    return result;
}
} // namespace monte_carlo_detail

// Request copies freeze distribution/options state before invoking user callbacks.
inline MonteCarloResult
run_monte_carlo(std::vector<MonteCarloParameter> parameters,
                std::size_t metric_count,
                const std::function<std::vector<double>(const std::vector<double> &)> &evaluate,
                MonteCarloOptions options = {},
                const std::function<bool()> &cancelled = {}) {
    monte_carlo_detail::validate(parameters, metric_count, options);
    if (!evaluate) {
        throw std::invalid_argument("Monte Carlo evaluator is empty");
    }
    MonteCarloResult result;
    result.seed = options.seed;
    result.requested_samples = options.samples;
    result.criteria_enabled = !options.criteria.empty();
    result.quantile_levels = options.quantile_levels;
    monte_carlo_detail::Random random(options.seed);
    std::vector<std::vector<double>> columns(metric_count);
    for (auto &column : columns) {
        column.reserve(options.samples);
    }
    if (options.record_history) {
        result.history.reserve(options.samples);
    }
    for (std::size_t index = 0; index < options.samples; ++index) {
        if (cancelled && cancelled()) {
            result.stop = MonteCarloStop::Cancelled;
            break;
        }
        MonteCarloSample sample;
        sample.index = index;
        sample.parameters.reserve(parameters.size());
        for (std::size_t j = 0; j < parameters.size(); ++j) {
            const double value = random.sample(parameters[j]);
            sample.parameters.push_back(value);
            if ((!std::isfinite(value) ||
                 (parameters[j].distribution == MonteCarloDistribution::LogNormal &&
                  value <= 0.)) &&
                sample.status == MonteCarloSampleStatus::Accepted) {
                sample.status = MonteCarloSampleStatus::SamplingFailure;
                sample.error = "unrepresentable sample at parameter " + std::to_string(j);
            }
        }
        ++result.samples_attempted;
        if (sample.status == MonteCarloSampleStatus::Accepted) {
            ++result.evaluations;
            try {
                auto values = evaluate(sample.parameters);
                if (values.size() != metric_count) {
                    sample.status = MonteCarloSampleStatus::InvalidMeasurementCount;
                    sample.error = "measurement dimension mismatch";
                } else if (!std::all_of(values.begin(), values.end(), [](double x) {
                               return std::isfinite(x);
                           })) {
                    sample.status = MonteCarloSampleStatus::NonFiniteMeasurement;
                    sample.error = "nonfinite measurement";
                } else {
                    sample.values = std::move(values);
                }
            } catch (const std::bad_alloc &) {
                throw;
            } catch (const std::exception &e) {
                sample.status = MonteCarloSampleStatus::Exception;
                // Bound diagnostic retention without first copying an arbitrary-length message.
                const char *message = e.what();
                for (std::size_t j = 0; message && j < 512 && message[j]; ++j) {
                    sample.error += message[j];
                }
            } catch (...) {
                sample.status = MonteCarloSampleStatus::Exception;
                sample.error = "nonstandard evaluator exception";
            }
        }
        const bool accepted = sample.status == MonteCarloSampleStatus::Accepted;
        if (accepted) {
            ++result.accepted_samples;
            sample.passed = result.criteria_enabled;
            for (std::size_t j = 0; j < metric_count; ++j) {
                columns[j].push_back(sample.values[j]);
                if (result.criteria_enabled) {
                    const auto &limit = options.criteria[j];
                    if ((limit.lower && sample.values[j] < *limit.lower) ||
                        (limit.upper && sample.values[j] > *limit.upper)) {
                        sample.passed = false;
                    }
                }
            }
            if (sample.passed) {
                ++result.passed_samples;
            }
        } else {
            ++result.rejected_samples;
            result.last_failed_parameters = sample.parameters;
            result.last_error = sample.error;
            if (options.invalid_sample == MonteCarloFailurePolicy::Stop) {
                result.stop = sample.status == MonteCarloSampleStatus::SamplingFailure
                                  ? MonteCarloStop::SamplingFailure
                                  : MonteCarloStop::EvaluationFailure;
            }
        }
        if (options.record_history) {
            result.history.push_back(std::move(sample));
        }
        if (!accepted && options.invalid_sample == MonteCarloFailurePolicy::Stop) {
            break;
        }
    }
    result.random_words = random.words;
    result.has_statistics = result.accepted_samples != 0;
    for (auto &column : columns) {
        result.statistics.push_back(monte_carlo_detail::summarize(column, options.quantile_levels));
    }
    if (result.criteria_enabled && result.samples_attempted) {
        result.passed_fraction = static_cast<double>(result.passed_samples) /
                                 static_cast<double>(result.samples_attempted);
    }
    if (result.criteria_enabled && result.accepted_samples) {
        result.valid_pass_fraction = static_cast<double>(result.passed_samples) /
                                     static_cast<double>(result.accepted_samples);
    }
    return result;
}
} // namespace rfmodel
