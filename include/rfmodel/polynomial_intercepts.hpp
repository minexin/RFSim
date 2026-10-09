#pragma once
#include "polynomial_limits.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace rfmodel {
enum class InterceptReference {
    input,
    output
};

struct TwoToneIntercept {
    int first_tone_order{};
    int second_tone_order{};
    double intercept_dbm{};
    int coefficient_sign{};
    InterceptReference reference{InterceptReference::input};
};

// Extrapolated, equal per-tone power intercept for one homogeneous product.
// The signed orders select k1*f1+k2*f2, with n=abs(k1)+abs(k2).
// They do not describe a sum of overlapping orders or compression at the IP.
inline std::vector<double>
polynomial_coefficients_from_intercepts(double power_gain_db,
                                        const std::vector<TwoToneIntercept> &intercepts,
                                        double reference_ohms = 50.) {
    if (!std::isfinite(power_gain_db) || !std::isfinite(reference_ohms) || reference_ohms <= 0. ||
        intercepts.size() > maximum_polynomial_order - 1) {
        throw std::invalid_argument("invalid polynomial intercept gain/reference/count");
    }
    const double log_ten = std::log(10.);
    const double log_gain = (power_gain_db / 20.) * log_ten;
    const double gain = std::exp(log_gain);
    if (!std::isfinite(gain) || gain <= 0.) {
        throw std::overflow_error("polynomial intercept gain is not representable");
    }
    std::vector<double> coefficients{0., gain};
    std::array<bool, maximum_polynomial_order + 1> used{};
    for (const auto &intercept : intercepts) {
        const int first = intercept.first_tone_order;
        const int second = intercept.second_tone_order;
        if (first == 0 || second == 0 || first < -10 || first > 10 || second < -10 || second > 10 ||
            !std::isfinite(intercept.intercept_dbm) ||
            (intercept.coefficient_sign != -1 && intercept.coefficient_sign != 1) ||
            (intercept.reference != InterceptReference::input &&
             intercept.reference != InterceptReference::output)) {
            throw std::invalid_argument("invalid two-tone intercept definition");
        }
        const int count_first = std::abs(first);
        const int count_second = std::abs(second);
        const int order = count_first + count_second;
        if (order > static_cast<int>(maximum_polynomial_order) || used[order]) {
            throw std::invalid_argument("duplicate or unsupported polynomial intercept order");
        }
        used[order] = true;
        // Binomial multiplicity for distinct tones, each appearing with one sign.
        double multiplicity = 1.;
        for (int i = 1; i <= count_second; ++i) {
            multiplicity *= static_cast<double>(count_first + i) / i;
        }
        const double input_dbm =
            intercept.intercept_dbm -
            (intercept.reference == InterceptReference::output ? power_gain_db : 0.);
        // Logarithms avoid overflow of intermediate R*P or high powers.
        const double log_voltage =
            .5 * (std::log(reference_ohms) - std::log(2.) + (input_dbm / 10. - 3.) * log_ten);
        const double magnitude =
            std::exp(log_gain - std::log(multiplicity) - (order - 1) * log_voltage);
        if (!std::isfinite(magnitude) || magnitude <= 0.) {
            throw std::overflow_error("polynomial intercept coefficient is not representable");
        }
        coefficients.resize(std::max(coefficients.size(), static_cast<std::size_t>(order + 1)), 0.);
        coefficients[order] = intercept.coefficient_sign * magnitude;
    }
    return coefficients;
}
} // namespace rfmodel
