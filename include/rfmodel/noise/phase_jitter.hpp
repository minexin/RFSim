#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rfmodel {
// One-sided PSD of a real phase process, not power-wave noise in W/Hz.
struct PhaseNoiseDensitySample {
    double offset_hz{};
    double phase_psd_rad2_per_hz{};
};

enum class PhaseJitterIntegrationPolicy {
    linear_rad2_per_hz = 0
};

struct PhaseJitterRequest {
    std::vector<PhaseNoiseDensitySample> samples;
    double carrier_hz;
    double lower_offset_hz;
    double upper_offset_hz;
    PhaseJitterIntegrationPolicy policy;

    // No implicit carrier, band, interpolation policy, or borrowed sample storage.
    PhaseJitterRequest(std::vector<PhaseNoiseDensitySample> samples_in,
                       double carrier_hz_in,
                       double lower_offset_hz_in,
                       double upper_offset_hz_in,
                       PhaseJitterIntegrationPolicy policy_in)
        : samples(std::move(samples_in)), carrier_hz(carrier_hz_in),
          lower_offset_hz(lower_offset_hz_in), upper_offset_hz(upper_offset_hz_in),
          policy(policy_in) {
    }
};

struct PhaseJitterMeasurement {
    double phase_variance_rad2{};
    double rms_phase_rad{};
    double rms_time_s{};
    double carrier_hz{};
    double lower_offset_hz{}, upper_offset_hz{};
    // Number of source intervals with a nonempty intersection, including zero PSD.
    std::size_t interpolation_intervals{};
    PhaseJitterIntegrationPolicy policy{PhaseJitterIntegrationPolicy::linear_rad2_per_hz};
};

namespace phase_jitter_detail {
// Positive binary-scaled arithmetic. Mantissas stay in [0.5,1), or zero;
// exponents for the bounded products below fit comfortably in int. This avoids
// overflow/underflow in intermediates without assuming extended long double.
struct Positive {
    double mantissa{};
    int exponent{};
};

inline Positive normalized(double mantissa, int exponent = 0) {
    if (mantissa == 0.) {
        return {};
    }
    int shift = 0;
    mantissa = std::frexp(mantissa, &shift);
    return {mantissa, exponent + shift};
}

inline Positive multiply(Positive a, Positive b) {
    return normalized(a.mantissa * b.mantissa, a.exponent + b.exponent);
}

inline Positive divide(Positive a, Positive b) {
    return normalized(a.mantissa / b.mantissa, a.exponent - b.exponent);
}

inline Positive add(Positive a, Positive b) {
    if (a.mantissa == 0.) {
        return b;
    }
    if (b.mantissa == 0.) {
        return a;
    }
    if (a.exponent < b.exponent) {
        std::swap(a, b);
    }
    return normalized(a.mantissa + std::scalbn(b.mantissa, b.exponent - a.exponent), a.exponent);
}

inline double normal_result(Positive value) {
    const double result = std::scalbn(value.mantissa, value.exponent);
    if (!std::isfinite(result) ||
        (value.mantissa != 0. && result < std::numeric_limits<double>::min())) {
        throw std::overflow_error("phase jitter result must be zero or finite normal");
    }
    return result;
}

class PositiveSum {
    double sum_{};
    double correction_{};
    int exponent_{};

public:
    void add_value(Positive value) {
        if (value.mantissa == 0.) {
            return;
        }
        if (sum_ == 0.) {
            sum_ = value.mantissa;
            exponent_ = value.exponent;
            return;
        }
        if (value.exponent > exponent_) {
            const int shift = exponent_ - value.exponent;
            sum_ = std::scalbn(sum_, shift);
            correction_ = std::scalbn(correction_, shift);
            exponent_ = value.exponent;
        }
        const double term = std::scalbn(value.mantissa, value.exponent - exponent_);
        const double next = sum_ + term;
        correction_ += sum_ >= term ? (sum_ - next) + term : (term - next) + sum_;
        sum_ = next;
    }

    Positive value() const {
        return normalized(sum_ + correction_, exponent_);
    }
};

inline Positive
density(const PhaseNoiseDensitySample &a, const PhaseNoiseDensitySample &b, double offset) {
    if (offset == a.offset_hz) {
        return normalized(a.phase_psd_rad2_per_hz);
    }
    if (offset == b.offset_hz) {
        return normalized(b.phase_psd_rad2_per_hz);
    }
    // Compute the two distances independently, rather than 1-t near a knot.
    const Positive span = normalized(b.offset_hz - a.offset_hz);
    const auto left =
        multiply(normalized(a.phase_psd_rad2_per_hz), normalized(b.offset_hz - offset));
    const auto right =
        multiply(normalized(b.phase_psd_rad2_per_hz), normalized(offset - a.offset_hz));
    return divide(add(left, right), span);
}
} // namespace phase_jitter_detail

// Explicit conventional small-angle PM conversion: one-sided S_phi = 2*10^(L/10).
// This does not extract phase from mixed AM/PM wave noise. Zero must be supplied
// as a linear PSD sample; infinite dBc/Hz values are not accepted.
inline double ssb_dbc_per_hz_to_phase_psd_rad2_per_hz(double ssb_dbc_per_hz) {
    if (!std::isfinite(ssb_dbc_per_hz)) {
        throw std::invalid_argument("SSB phase noise dBc/Hz must be finite");
    }
    // Bound before converting an exponent to int. No finite-normal result is
    // possible outside this deliberately loose input range.
    if (ssb_dbc_per_hz < -4000. || ssb_dbc_per_hz > 4000.) {
        throw std::overflow_error("converted phase PSD must be finite normal");
    }
    // Split log2(10)/10 and retain the multiplication residual. Evaluating a
    // single large natural-log exponent loses accuracy near the normal boundary.
    constexpr double log2_10_over_10_hi = .33219280948873625;
    constexpr double log2_10_over_10_lo = -1.66905155690187749e-17;
    const double exponent = ssb_dbc_per_hz * log2_10_over_10_hi;
    const double residual = std::fma(ssb_dbc_per_hz, log2_10_over_10_hi, -exponent) +
                            ssb_dbc_per_hz * log2_10_over_10_lo;
    const double integral = std::floor(exponent);
    const double fraction = (exponent - integral) + residual;
    const double value = std::scalbn(std::exp2(fraction), static_cast<int>(integral) + 1);
    if (!std::isfinite(value) || value < std::numeric_limits<double>::min()) {
        throw std::overflow_error("converted phase PSD must be finite normal");
    }
    return value;
}

// Integrates only the supplied linear-in-Hz, linear-in-rad^2/Hz interpolant.
// No extrapolation, default band, logarithmic interpolation, or inferred carrier.
// The PSD process need not have upper_offset_hz < carrier_hz. Large variance is
// numerically permitted; it does not establish small-angle PM model validity.
inline PhaseJitterMeasurement measure_phase_jitter(const PhaseJitterRequest &request) {
    using namespace phase_jitter_detail;
    const auto &samples = request.samples;
    if (samples.size() < 2 || samples.size() > 1000000 ||
        request.policy != PhaseJitterIntegrationPolicy::linear_rad2_per_hz ||
        !std::isfinite(request.carrier_hz) || request.carrier_hz <= 0. ||
        !std::isfinite(request.lower_offset_hz) || request.lower_offset_hz <= 0. ||
        !std::isfinite(request.upper_offset_hz) ||
        request.upper_offset_hz <= request.lower_offset_hz) {
        throw std::invalid_argument("invalid phase jitter samples, carrier, band or policy");
    }
    // Validate the entire table before integrating, including samples outside the band.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto &sample = samples[i];
        if (!std::isfinite(sample.offset_hz) || sample.offset_hz <= 0. ||
            !std::isfinite(sample.phase_psd_rad2_per_hz) || sample.phase_psd_rad2_per_hz < 0. ||
            (i != 0 && sample.offset_hz <= samples[i - 1].offset_hz)) {
            throw std::invalid_argument(
                "phase PSD samples must be finite, nonnegative and ordered");
        }
    }
    if (samples.front().offset_hz > request.lower_offset_hz ||
        samples.back().offset_hz < request.upper_offset_hz) {
        throw std::out_of_range("phase PSD samples must cover the complete requested band");
    }
    PhaseJitterMeasurement result;
    result.carrier_hz = request.carrier_hz;
    result.lower_offset_hz = request.lower_offset_hz;
    result.upper_offset_hz = request.upper_offset_hz;
    result.policy = request.policy;
    PositiveSum total;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto &a = samples[i - 1];
        const auto &b = samples[i];
        const double low = std::max(a.offset_hz, request.lower_offset_hz);
        const double high = std::min(b.offset_hz, request.upper_offset_hz);
        if (low >= high) {
            continue;
        }
        auto mean = add(density(a, b, low), density(a, b, high));
        --mean.exponent; // divide by two without losing a subnormal endpoint
        total.add_value(multiply(mean, normalized(high - low)));
        ++result.interpolation_intervals;
    }
    result.phase_variance_rad2 = normal_result(total.value());
    result.rms_phase_rad = std::sqrt(result.phase_variance_rad2);
    // Do not form 2*pi*carrier: it can overflow for a representable final time.
    result.rms_time_s = normal_result(
        divide(divide(normalized(result.rms_phase_rad), normalized(request.carrier_hz)),
               normalized(2. * std::acos(-1.))));
    return result;
}
} // namespace rfmodel
