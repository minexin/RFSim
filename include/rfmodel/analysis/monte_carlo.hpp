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
    // Empty retains the original independent stream; otherwise row-major latent
    // Gaussian correlation in parameter order, including constant marginals.
    std::vector<double> latent_correlation;
    bool joint_statistics{false};
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

struct MonteCarloJointStatistics {
    std::size_t dimension{}, count{};
    // Row-major; unavailable entries (n < 2, overflow, zero-variance correlation)
    // are nullopt. All columns use the same accepted sample rows.
    std::vector<std::optional<double>> sample_covariance, sample_correlation;
};

struct MonteCarloResult {
    std::string algorithm{"mt19937_64-box-muller52-v1"};
    std::size_t latent_rank{};
    std::vector<double> effective_latent_correlation;
    MonteCarloJointStatistics parameter_joint, metric_joint;
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

inline constexpr const char *monte_carlo_copula_algorithm =
    "mt19937_64-box-muller52-gaussian-copula-pivoted-v1";

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

// Pivoted PSD Cholesky in a deterministic order (first maximum wins).
// A roundoff-size final residual may be discarded; the normalized factor's
// actual correlation is returned so this numerical adjustment is observable.
struct Copula {
    std::size_t dimension{}, rank{};
    std::vector<double> factor, effective;

    explicit Copula(const std::vector<double> &matrix, std::size_t n) {
        if (matrix.empty()) {
            return;
        }
        if (matrix.size() != n * n) {
            throw std::invalid_argument("latent correlation dimension mismatch");
        }
        dimension = n;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                const auto x = matrix[i * n + j];
                if (!std::isfinite(x) || std::abs(x) > 1. || x != matrix[j * n + i] ||
                    (i == j && x != 1.)) {
                    throw std::invalid_argument("latent correlation must be finite, symmetric, "
                                                "unit diagonal and in [-1,1]");
                }
            }
        }
        auto work = matrix;
        std::vector<double> lower(n * n);
        std::vector<std::size_t> order(n);
        for (std::size_t i = 0; i < n; ++i) {
            order[i] = i;
        }
        const double tolerance =
            64. * static_cast<double>(n) * std::numeric_limits<double>::epsilon();
        for (std::size_t k = 0; k < n; ++k) {
            std::size_t pivot = k;
            for (std::size_t i = k + 1; i < n; ++i) {
                if (work[i * n + i] > work[pivot * n + pivot]) {
                    pivot = i;
                }
            }
            if (work[pivot * n + pivot] <= tolerance) {
                for (std::size_t i = k; i < n; ++i) {
                    for (std::size_t j = k; j < n; ++j) {
                        if (std::abs(work[i * n + j]) > tolerance) {
                            throw std::invalid_argument(
                                "latent correlation is not positive semidefinite");
                        }
                    }
                }
                break;
            }
            for (std::size_t j = 0; j < n; ++j) {
                std::swap(work[k * n + j], work[pivot * n + j]);
            }
            for (std::size_t i = 0; i < n; ++i) {
                std::swap(work[i * n + k], work[i * n + pivot]);
            }
            for (std::size_t j = 0; j < k; ++j) {
                std::swap(lower[k * n + j], lower[pivot * n + j]);
            }
            std::swap(order[k], order[pivot]);
            lower[k * n + k] = std::sqrt(work[k * n + k]);
            for (std::size_t i = k + 1; i < n; ++i) {
                lower[i * n + k] = work[i * n + k] / lower[k * n + k];
            }
            for (std::size_t i = k + 1; i < n; ++i) {
                for (std::size_t j = k + 1; j < n; ++j) {
                    work[i * n + j] -= lower[i * n + k] * lower[j * n + k];
                }
            }
            ++rank;
        }
        factor.resize(n * n);
        for (std::size_t i = 0; i < n; ++i) {
            double norm = 0.;
            for (std::size_t k = 0; k < rank; ++k) {
                norm = std::hypot(norm, lower[i * n + k]);
            }
            if (!(norm > 0.) || !std::isfinite(norm)) {
                throw std::invalid_argument("latent correlation factor is invalid");
            }
            for (std::size_t k = 0; k < rank; ++k) {
                factor[order[i] * n + k] = lower[i * n + k] / norm;
            }
        }
        effective.resize(n * n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                double value = 0.;
                for (std::size_t k = 0; k < rank; ++k) {
                    value += factor[i * n + k] * factor[j * n + k];
                }
                effective[i * n + j] = value;
                if (std::abs(value - matrix[i * n + j]) > 4. * tolerance) {
                    throw std::invalid_argument("latent correlation reconstruction is inaccurate");
                }
            }
        }
    }
};

inline double copula_marginal(const MonteCarloParameter &p, double z) {
    if (p.distribution == MonteCarloDistribution::Fixed) {
        return p.first;
    }
    if (p.distribution == MonteCarloDistribution::Normal ||
        p.distribution == MonteCarloDistribution::LogNormal) {
        const double value = std::fma(p.second, z, p.first);
        return p.distribution == MonteCarloDistribution::Normal ? value : std::exp(value);
    }
    if (p.first == p.second) {
        return p.first;
    }
    const double u = .5 * std::erfc(-z / std::sqrt(2.));
    if (p.distribution == MonteCarloDistribution::Uniform) {
        return interpolate(p.first, p.second, u);
    }
    if (p.distribution == MonteCarloDistribution::LogUniform) {
        return std::clamp(
            std::exp(interpolate(std::log(p.first), std::log(p.second), u)), p.first, p.second);
    }
    const auto low = static_cast<std::int64_t>(p.first);
    const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(p.second) - low) + 1;
    const auto bin =
        std::min(static_cast<std::uint64_t>(u * static_cast<double>(width)), width - 1);
    return static_cast<double>(low + static_cast<std::int64_t>(bin));
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

// Rescale a normalized covariance without overflowing intermediate products.
inline double scaled_product(double x, double a, double b) {
    if (x == 0. || a == 0. || b == 0.) {
        return 0.;
    }
    int ex = 0, ea = 0, eb = 0;
    const double mx = std::frexp(x, &ex), ma = std::frexp(a, &ea), mb = std::frexp(b, &eb);
    return std::ldexp(mx * ma * mb, ex + ea + eb);
}

inline MonteCarloJointStatistics summarize_joint(const std::vector<std::vector<double>> &columns) {
    MonteCarloJointStatistics out;
    out.dimension = columns.size();
    out.count = columns.empty() ? 0 : columns[0].size();
    const auto n = out.dimension;
    out.sample_covariance.resize(n * n);
    out.sample_correlation.resize(n * n);
    if (out.count < 2) {
        return out;
    }
    std::vector<double> centers(n), scales(n), means(n), deviations(n);
    for (std::size_t j = 0; j < n; ++j) {
        const auto bounds = std::minmax_element(columns[j].begin(), columns[j].end());
        centers[j] = interpolate(*bounds.first, *bounds.second, .5);
        scales[j] =
            std::max(std::abs(*bounds.first - centers[j]), std::abs(*bounds.second - centers[j]));
        if (scales[j] == 0.) {
            continue;
        }
        Sum sum;
        for (double value : columns[j]) {
            sum.add((value - centers[j]) / scales[j]);
        }
        means[j] = sum.value / static_cast<double>(out.count);
        Sum squares;
        for (double value : columns[j]) {
            const double d = (value - centers[j]) / scales[j] - means[j];
            squares.add(d * d);
        }
        deviations[j] = std::sqrt(squares.value / static_cast<double>(out.count - 1));
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            Sum products;
            if (scales[i] != 0. && scales[j] != 0.) {
                for (std::size_t k = 0; k < out.count; ++k) {
                    products.add(((columns[i][k] - centers[i]) / scales[i] - means[i]) *
                                 ((columns[j][k] - centers[j]) / scales[j] - means[j]));
                }
            }
            const double normalized = products.value / static_cast<double>(out.count - 1);
            const double sd = scales[i] * deviations[i];
            const auto cov =
                representable(i == j ? sd * sd : scaled_product(normalized, scales[i], scales[j]));
            out.sample_covariance[i * n + j] = out.sample_covariance[j * n + i] = cov;
            if (deviations[i] > 0. && deviations[j] > 0.) {
                const double corr =
                    i == j ? 1. : std::clamp(normalized / deviations[i] / deviations[j], -1., 1.);
                out.sample_correlation[i * n + j] = out.sample_correlation[j * n + i] = corr;
            }
        }
    }
    return out;
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
    const monte_carlo_detail::Copula copula(options.latent_correlation, parameters.size());
    MonteCarloResult result;
    if (copula.dimension) {
        result.algorithm = monte_carlo_copula_algorithm;
        result.latent_rank = copula.rank;
        result.effective_latent_correlation = copula.effective;
    }
    result.seed = options.seed;
    result.requested_samples = options.samples;
    result.criteria_enabled = !options.criteria.empty();
    result.quantile_levels = options.quantile_levels;
    monte_carlo_detail::Random random(options.seed);
    std::vector<std::vector<double>> columns(metric_count);
    std::vector<std::vector<double>> parameter_columns(options.joint_statistics ? parameters.size()
                                                                                : 0);
    for (auto &column : parameter_columns) {
        column.reserve(options.samples);
    }
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
        std::vector<double> latent(copula.dimension);
        for (auto &value : latent) {
            value = random.normal();
        }
        for (std::size_t j = 0; j < parameters.size(); ++j) {
            double z = 0.;
            for (std::size_t k = 0; k < copula.rank; ++k) {
                z += copula.factor[j * copula.dimension + k] * latent[k];
            }
            const double value = copula.dimension
                                     ? monte_carlo_detail::copula_marginal(parameters[j], z)
                                     : random.sample(parameters[j]);
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
            for (std::size_t j = 0; j < parameter_columns.size(); ++j) {
                parameter_columns[j].push_back(sample.parameters[j]);
            }
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
    // Compute joint moments before marginal quantiles sort the columns.
    if (options.joint_statistics) {
        result.parameter_joint = monte_carlo_detail::summarize_joint(parameter_columns);
        result.metric_joint = monte_carlo_detail::summarize_joint(columns);
    }
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
