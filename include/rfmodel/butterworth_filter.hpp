#pragma once
#include "device_model.hpp"
#include "filter_frequency.hpp"
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
        log_frequency_scale_ = detail::log_excess_power(parameters_.passband_attenuation_db) /
                               (2. * double(parameters_.order));
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
        const auto mapped =
            detail::map_filter_frequency(static_cast<FilterResponse>(parameters_.response),
                                         parameters_.lower_passband_hz,
                                         parameters_.upper_passband_hz,
                                         frequency_hz);
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
