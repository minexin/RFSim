#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace rfmodel {
struct NoiseDensitySample {
    double frequency_hz{};
    double watts_per_hz{};
};

// One already-combined deterministic spectral line, in RMS power-wave watts.
struct ChannelSignalLine {
    double frequency_hz{};
    double power_w{};
};

enum class ChannelRatioState {
    finite = 0,
    noise_free = 1,
    no_signal = 2,
    empty = 3
};

struct ChannelNoiseMeasurement {
    double lower_frequency_hz{}, upper_frequency_hz{}, effective_bandwidth_hz{};
    double noise_power_w{}, mean_noise_density_w_per_hz{}, desired_signal_power_w{};
    std::optional<double> carrier_to_noise_db;
    ChannelRatioState ratio_state{ChannelRatioState::empty};
    std::size_t interpolation_intervals{}, desired_line_count{};
};

// Linear interpolation is in W/Hz, followed by exact trapezoidal integration of
// that interpolant. No extrapolation, no assumption that sparse samples are flat.
inline ChannelNoiseMeasurement
measure_channel_noise(const std::vector<NoiseDensitySample> &noise,
                      const std::vector<ChannelSignalLine> &desired_lines,
                      double center_hz,
                      double bandwidth_hz) {
    if (noise.size() < 2 || noise.size() > 1000000 || desired_lines.size() > 1000000 ||
        !std::isfinite(center_hz) || center_hz < 0. || !std::isfinite(bandwidth_hz) ||
        bandwidth_hz <= 0.) {
        throw std::invalid_argument("invalid channel noise grid or band");
    }
    ChannelNoiseMeasurement result;
    result.lower_frequency_hz = std::max(0., center_hz - bandwidth_hz / 2.);
    result.upper_frequency_hz = center_hz + bandwidth_hz / 2.;
    result.effective_bandwidth_hz = result.upper_frequency_hz - result.lower_frequency_hz;
    if (!std::isfinite(result.upper_frequency_hz) || result.effective_bandwidth_hz <= 0.) {
        throw std::invalid_argument("channel edges are not representable");
    }
    for (std::size_t i = 0; i < noise.size(); ++i) {
        const auto &sample = noise[i];
        if (!std::isfinite(sample.frequency_hz) || sample.frequency_hz < 0. ||
            !std::isfinite(sample.watts_per_hz) || sample.watts_per_hz < 0. ||
            (i && sample.frequency_hz <= noise[i - 1].frequency_hz)) {
            throw std::invalid_argument("noise samples must be finite, nonnegative and ordered");
        }
    }
    if (noise.front().frequency_hz > result.lower_frequency_hz ||
        noise.back().frequency_hz < result.upper_frequency_hz) {
        throw std::out_of_range("noise samples must cover the complete clipped channel band");
    }
    // Neumaier accumulation retains small positive contributions over large bands.
    double sum = 0., correction = 0.;
    auto accumulate = [&](double value) {
        const double next = sum + value;
        if (!std::isfinite(value) || !std::isfinite(next)) {
            throw std::overflow_error("integrated channel power overflow");
        }
        correction +=
            std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
        sum = next;
    };
    for (std::size_t i = 1; i < noise.size(); ++i) {
        const auto &first = noise[i - 1];
        const auto &second = noise[i];
        const double low = std::max(first.frequency_hz, result.lower_frequency_hz);
        const double high = std::min(second.frequency_hz, result.upper_frequency_hz);
        if (high <= low) {
            continue;
        }
        auto density = [&](double frequency) {
            const double t =
                (frequency - first.frequency_hz) / (second.frequency_hz - first.frequency_hz);
            const double a = first.watts_per_hz, b = second.watts_per_hz;
            const double value = a <= b ? a + t * (b - a) : b + (1. - t) * (a - b);
            if (value == 0. && t > 0. && t < 1. && (a > 0. || b > 0.)) {
                throw std::overflow_error("interpolated channel density underflow");
            }
            return value;
        };
        const double left = density(low), right = density(high);
        const double mean = std::min(left, right) + .5 * std::abs(right - left);
        const double area = (high - low) * mean;
        if (mean > 0. && area == 0.) {
            throw std::overflow_error("integrated channel noise underflow");
        }
        accumulate(area);
        ++result.interpolation_intervals;
    }
    result.noise_power_w = sum + correction;
    result.mean_noise_density_w_per_hz = result.noise_power_w / result.effective_bandwidth_hz;
    if (!std::isfinite(result.noise_power_w) ||
        !std::isfinite(result.mean_noise_density_w_per_hz)) {
        throw std::overflow_error("channel noise result overflow");
    }
    sum = 0.;
    correction = 0.;
    for (std::size_t i = 0; i < desired_lines.size(); ++i) {
        const auto &line = desired_lines[i];
        if (!std::isfinite(line.frequency_hz) || line.frequency_hz < 0. ||
            !std::isfinite(line.power_w) || line.power_w < 0. ||
            (i && line.frequency_hz <= desired_lines[i - 1].frequency_hz)) {
            throw std::invalid_argument("desired lines must be finite, nonnegative and ordered");
        }
        if (line.frequency_hz >= result.lower_frequency_hz &&
            line.frequency_hz <= result.upper_frequency_hz) {
            accumulate(line.power_w);
            ++result.desired_line_count;
        }
    }
    result.desired_signal_power_w = sum + correction;
    if (!std::isfinite(result.desired_signal_power_w)) {
        throw std::overflow_error("desired channel power overflow");
    }
    if (result.noise_power_w > 0. && result.desired_signal_power_w > 0.) {
        result.ratio_state = ChannelRatioState::finite;
        result.carrier_to_noise_db =
            10. * (std::log10(result.desired_signal_power_w) - std::log10(result.noise_power_w));
    } else if (result.desired_signal_power_w > 0.) {
        result.ratio_state = ChannelRatioState::noise_free;
    } else if (result.noise_power_w > 0.) {
        result.ratio_state = ChannelRatioState::no_signal;
    }
    return result;
}
} // namespace rfmodel
