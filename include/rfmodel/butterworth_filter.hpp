#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
enum class ButterworthResponse {
    Lowpass,
    Highpass,
    Bandpass,
    Bandstop
};

struct ButterworthFilterParameters {
    ButterworthResponse response = ButterworthResponse::Lowpass;
    std::size_t order = 3;          // Lowpass prototype order; band transforms double it.
    double lower_passband_hz = 1e9; // Single passband edge for low/high pass.
    double upper_passband_hz = 0.;
    double passband_attenuation_db = 3.010299956639812;
    bool input_stopband_open = true;
    double reference_ohms = 50.;
};

// Lossless, reciprocal Butterworth LC ladder. No empirical insertion loss or
// finite-stopband leakage is implied by this ideal synthesis.
class ButterworthFilterModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    ButterworthFilterParameters parameters_;
    double log_frequency_scale_;

    struct MappedFrequency {
        double sign;
        double log_magnitude;
    };

    static double log_sum(double a, double b) {
        const double high = std::max(a, b), low = std::min(a, b);
        return high + std::log1p(std::exp(low - high));
    }

    MappedFrequency mapped_frequency(double frequency) const {
        const auto kind = parameters_.response;
        const double infinity = std::numeric_limits<double>::infinity();
        if (frequency == 0.) {
            const bool pass =
                kind == ButterworthResponse::Lowpass || kind == ButterworthResponse::Bandstop;
            return {pass ? 1. : -1., pass ? -infinity : infinity};
        }
        const double low = parameters_.lower_passband_hz;
        if (kind == ButterworthResponse::Lowpass) {
            return {1., std::log(frequency) - std::log(low)};
        }
        if (kind == ButterworthResponse::Highpass) {
            return {-1., std::log(low) - std::log(frequency)};
        }
        const double high = parameters_.upper_passband_hz;
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
            const double y = (frequency - low) / bandwidth -
                             (low / frequency) * ((high - frequency) / bandwidth);
            sign = y < 0. ? -1. : 1.;
            magnitude = y == 0. ? -infinity : std::log(std::abs(y));
        }
        if (kind == ButterworthResponse::Bandstop) {
            return {-sign, -magnitude}; // Lowpass variable is -1/y.
        }
        return {sign, magnitude};
    }

public:
    ButterworthFilterModel(std::string name, ButterworthFilterParameters parameters)
        : name_(std::move(name)), parameters_(parameters) {
        const auto kind = parameters_.response;
        const bool band =
            kind == ButterworthResponse::Bandpass || kind == ButterworthResponse::Bandstop;
        if (name_.empty() || parameters_.order < 2 || parameters_.order > 64 ||
            (kind != ButterworthResponse::Lowpass && kind != ButterworthResponse::Highpass &&
             !band) ||
            !std::isfinite(parameters_.lower_passband_hz) || parameters_.lower_passband_hz <= 0. ||
            !std::isfinite(parameters_.upper_passband_hz) ||
            (band ? parameters_.upper_passband_hz <= parameters_.lower_passband_hz
                  : parameters_.upper_passband_hz != 0.) ||
            !std::isfinite(parameters_.passband_attenuation_db) ||
            parameters_.passband_attenuation_db <= 0. ||
            !std::isfinite(parameters_.reference_ohms) || parameters_.reference_ohms <= 0.) {
            throw std::invalid_argument("invalid Butterworth ladder parameters");
        }
        constexpr double db_scale = 0.2302585092994045684;
        const double x = parameters_.passband_attenuation_db * db_scale;
        double logarithm;
        if (x < 1e-4) {
            logarithm = std::log(parameters_.passband_attenuation_db) + std::log(db_scale);
            if (x > 0.) {
                logarithm += std::log(std::expm1(x) / x);
            }
        } else {
            logarithm = x > 700. ? x : std::log(std::expm1(x));
        }
        log_frequency_scale_ = logarithm / (2. * double(parameters_.order));
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= 2) {
            throw std::out_of_range("Butterworth filter port");
        }
        return {index, index == 0 ? "in" : "out", {parameters_.reference_ohms, 0.}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0.) {
            throw std::invalid_argument("invalid Butterworth frequency");
        }
        const auto mapped = mapped_frequency(frequency_hz);
        const double log_omega = mapped.log_magnitude + log_frequency_scale_;
        const Complex phase{0., mapped.sign};
        SMatrix result{2, {0., 1., 1., 0.}};
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t index = 0; index < parameters_.order; ++index) {
            // g_k=2*sin((2k-1)*pi/(2N)); q=j*g_k*Omega/2.
            const double coefficient =
                std::sin((2. * double(index) + 1.) * pi / (2. * double(parameters_.order)));
            const double log_q = log_omega + std::log(coefficient);
            const bool series = (index % 2 == 0) == parameters_.input_stopband_open;
            Complex transmission, reflection;
            if (log_q > 0.) {
                const double inverse = std::exp(-log_q);
                transmission = inverse / (inverse + phase);
                reflection = (series ? 1. : -1.) * phase / (inverse + phase);
            } else {
                const Complex q = std::exp(log_q) * phase;
                transmission = 1. / (1. + q);
                reflection = (series ? 1. : -1.) * q / (1. + q);
            }
            const Complex denominator = 1. - result(1, 1) * reflection;
            if (denominator == Complex{}) {
                throw std::domain_error("singular Butterworth ladder cascade");
            }
            const Complex forward = result(1, 0) * transmission / denominator;
            const Complex input =
                result(0, 0) + result(0, 1) * reflection * result(1, 0) / denominator;
            const Complex output =
                reflection + transmission * result(1, 1) * transmission / denominator;
            result = {2, {input, forward, forward, output}};
        }
        for (auto value : result.values) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                throw std::overflow_error("Butterworth scattering overflow");
            }
        }
        return result;
    }
};
} // namespace rfmodel
