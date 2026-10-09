#pragma once
#include "device_model.hpp"
#include "filter_frequency.hpp"
#include <array>
#include <vector>

namespace rfmodel {
struct ChebyshevFilterParameters {
    FilterResponse response = FilterResponse::Lowpass;
    std::size_t order = 3;
    double lower_passband_hz = 1e9;
    double upper_passband_hz = 0.;
    double ripple_db = 0.1;
    double passband_attenuation_db = 0.1;
    bool input_stopband_open = true;
    double reference_ohms = 50.;
};

// Lossless reciprocal type-I response synthesized from the stable poles and
// a Chebyshev reflection polynomial. Even orders retain their DC mismatch.
class ChebyshevFilterModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    ChebyshevFilterParameters parameters_;
    double log_epsilon_, log_frequency_scale_;
    std::vector<Complex> poles_;

    static double log_cosh(double x) {
        return x + std::log1p(std::exp(-2. * x)) - std::log(2.);
    }

public:
    ChebyshevFilterModel(std::string name, ChebyshevFilterParameters parameters)
        : name_(std::move(name)), parameters_(parameters) {
        const auto &p = parameters_;
        const bool band =
            p.response == FilterResponse::Bandpass || p.response == FilterResponse::Bandstop;
        if (name_.empty() || p.order < 2 || p.order > 64 ||
            (p.response != FilterResponse::Lowpass && p.response != FilterResponse::Highpass &&
             !band) ||
            !std::isfinite(p.lower_passband_hz) || p.lower_passband_hz <= 0. ||
            !std::isfinite(p.upper_passband_hz) ||
            (band ? p.upper_passband_hz <= p.lower_passband_hz : p.upper_passband_hz != 0.) ||
            !std::isfinite(p.ripple_db) || p.ripple_db <= 0. ||
            !std::isfinite(p.passband_attenuation_db) || p.passband_attenuation_db < p.ripple_db ||
            !std::isfinite(p.reference_ohms) || p.reference_ohms <= 0.) {
            throw std::invalid_argument("invalid Chebyshev type-I parameters");
        }
        log_epsilon_ = 0.5 * detail::log_excess_power(p.ripple_db);
        const double mu = std::asinh(std::exp(-log_epsilon_)) / double(p.order);
        if (mu == 0. || !std::isfinite(mu)) {
            throw std::invalid_argument("Chebyshev ripple exceeds numerical range");
        }
        const double log_ratio = std::max(
            0., 0.5 * (detail::log_excess_power(p.passband_attenuation_db) - 2. * log_epsilon_));
        // acosh(exp(x)) without forming exp(x), then log(cosh(acosh(...)/N)).
        const double edge =
            (log_ratio + std::log1p(std::sqrt(-std::expm1(-2. * log_ratio)))) / double(p.order);
        log_frequency_scale_ = log_cosh(edge);
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t k = 0; k < p.order; ++k) {
            const std::size_t mirrored = std::min(k, p.order - 1 - k);
            const double theta = (2. * double(mirrored) + 1.) * pi / (2. * double(p.order));
            const double imaginary = 2 * k + 1 == p.order ? 0.
                                                          : (2 * k < p.order ? 1. : -1.) *
                                                                std::cosh(mu) * std::cos(theta);
            const double real = -std::sinh(mu) * std::sin(theta);
            if (real == 0.) {
                throw std::invalid_argument("Chebyshev pole exceeds numerical range");
            }
            poles_.push_back({real, imaginary});
        }
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= 2) {
            throw std::out_of_range("Chebyshev filter port");
        }
        return {index, index == 0 ? "in" : "out", {parameters_.reference_ohms, 0.}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0.) {
            throw std::invalid_argument("invalid Chebyshev frequency");
        }
        const auto mapped = detail::map_filter_frequency(parameters_.response,
                                                         parameters_.lower_passband_hz,
                                                         parameters_.upper_passband_hz,
                                                         frequency_hz);
        const double log_omega = mapped.log_magnitude + log_frequency_scale_;
        const double order = double(parameters_.order);
        double log_polynomial, polynomial_sign;
        if (log_omega <= 0.) {
            const double square = std::exp(2. * log_omega);
            // Keep odd polynomials divided by Omega. This avoids cancellation
            // near DC and retains epsilon*T_N when Omega itself underflows.
            double previous = 1., polynomial = 1.;
            for (std::size_t k = 2; k <= parameters_.order; ++k) {
                const double next = 2. * (k % 2 ? 1. : square) * polynomial - previous;
                previous = polynomial;
                polynomial = next;
            }
            polynomial_sign = polynomial < 0. ? -1. : 1.;
            log_polynomial =
                std::log(std::abs(polynomial)) + (parameters_.order % 2 ? log_omega : 0.);
        } else {
            const double argument = log_omega + std::log1p(std::sqrt(-std::expm1(-2. * log_omega)));
            log_polynomial = log_cosh(order * argument);
            polynomial_sign = 1.;
        }
        if (mapped.sign < 0. && parameters_.order % 2) {
            polynomial_sign = -polynomial_sign;
        }
        const double log_ratio = log_epsilon_ + log_polynomial;
        double transmitted, reflected;
        if (log_ratio > 0.) {
            const double inverse = std::exp(-log_ratio);
            reflected = 1. / std::hypot(1., inverse);
            transmitted = inverse * reflected;
        } else {
            const double ratio = std::exp(log_ratio);
            transmitted = 1. / std::hypot(1., ratio);
            reflected = ratio * transmitted;
        }
        Complex phase = 1.;
        // Normalize each pole factor before multiplying: no high-order products
        // or huge frequency powers, while preserving the causal transfer phase.
        if (log_omega != -std::numeric_limits<double>::infinity()) {
            for (const auto pole : poles_) {
                if (log_omega == std::numeric_limits<double>::infinity()) {
                    phase *= Complex{0., -mapped.sign};
                    continue;
                }
                const double pole_magnitude = std::abs(pole);
                const double pole_log = std::log(pole_magnitude);
                const double scale = std::max(log_omega, pole_log);
                const Complex denominator = Complex{0., mapped.sign * std::exp(log_omega - scale)} -
                                            (pole / pole_magnitude) * std::exp(pole_log - scale);
                phase *= std::conj(denominator) / std::abs(denominator);
            }
            phase /= std::abs(phase);
        }
        const std::array<Complex, 4> rotations{1., Complex{0., 1.}, -1., Complex{0., -1.}};
        const Complex transmission = transmitted * phase;
        const Complex input = (parameters_.input_stopband_open ? 1. : -1.) * polynomial_sign *
                              reflected * rotations[parameters_.order % 4] * phase;
        const Complex output = (parameters_.order % 2 ? 1. : -1.) * input;
        return {2, {input, transmission, transmission, output}};
    }
};
} // namespace rfmodel
