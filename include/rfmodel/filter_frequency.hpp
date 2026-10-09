#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
enum class FilterResponse {
    Lowpass,
    Highpass,
    Bandpass,
    Bandstop
};

namespace detail {
struct MappedFrequency {
    double sign;
    double log_magnitude;
};

inline double log_sum(double a, double b) {
    const double high = std::max(a, b), low = std::min(a, b);
    return high + std::log1p(std::exp(low - high));
}

inline MappedFrequency
map_filter_frequency(FilterResponse kind, double low, double high, double frequency) {
    const double infinity = std::numeric_limits<double>::infinity();
    if (frequency == 0.) {
        const bool pass = kind == FilterResponse::Lowpass || kind == FilterResponse::Bandstop;
        return {pass ? 1. : -1., pass ? -infinity : infinity};
    }
    if (kind == FilterResponse::Lowpass) {
        return {1., std::log(frequency) - std::log(low)};
    }
    if (kind == FilterResponse::Highpass) {
        return {-1., std::log(low) - std::log(frequency)};
    }
    const double bandwidth = high - low;
    double sign, magnitude;
    // y=(f^2-low*high)/(bandwidth*f). Differences avoid cancellation for
    // narrow bands; logarithms outside the band avoid overflow.
    if (frequency < low) {
        sign = -1.;
        magnitude = log_sum(std::log(low - frequency) - std::log(bandwidth),
                            std::log(low) - std::log(frequency) + std::log(high - frequency) -
                                std::log(bandwidth));
    } else if (frequency > high) {
        sign = 1.;
        magnitude = log_sum(std::log(frequency - low) - std::log(bandwidth),
                            std::log(low) - std::log(frequency) + std::log(frequency - high) -
                                std::log(bandwidth));
    } else {
        const double y =
            (frequency - low) / bandwidth - (low / frequency) * ((high - frequency) / bandwidth);
        sign = y < 0. ? -1. : 1.;
        magnitude = y == 0. ? -infinity : std::log(std::abs(y));
    }
    if (kind == FilterResponse::Bandstop) {
        return {-sign, -magnitude}; // Lowpass variable is -1/y.
    }
    return {sign, magnitude};
}

inline double log_excess_power(double attenuation_db) {
    constexpr double scale = 0.2302585092994045684;
    const double x = attenuation_db * scale;
    if (x < 1e-4) {
        const double correction = x > 0. ? std::log(std::expm1(x) / x) : 0.;
        return std::log(attenuation_db) + std::log(scale) + correction;
    }
    return x > 700. ? x : std::log(std::expm1(x));
}
} // namespace detail
} // namespace rfmodel
