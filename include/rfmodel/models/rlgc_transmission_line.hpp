#pragma once
#include "rfmodel/core/device_model.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace rfmodel {
struct RlgcPerLength {
    double resistance_ohms_per_m{};
    double inductance_h_per_m{};
    double conductance_s_per_m{};
    double capacitance_f_per_m{};
};

// Uniform distributed line. R,L,G,C are constant coefficients, not lumped totals.
class RlgcTransmissionLineModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    RlgcPerLength coefficients_;
    double length_, reference_;

    static bool finite(Complex value) {
        return std::isfinite(value.real()) && std::isfinite(value.imag());
    }

    // Keep the exponent separate until every positive factor (and the divisor)
    // has participated. In particular, zero must not become zero * infinity.
    static double normalized_product(std::initializer_list<double> factors, double divisor = 1.) {
        double mantissa = 1.;
        int exponent = 0;
        for (double factor : factors) {
            if (factor == 0.) {
                return 0.;
            }
            int factor_exponent;
            mantissa *= std::frexp(factor, &factor_exponent);
            exponent += factor_exponent;
        }
        int divisor_exponent;
        mantissa /= std::frexp(divisor, &divisor_exponent);
        return std::scalbn(mantissa, exponent - divisor_exponent);
    }

    static Complex electrical_length(Complex series, Complex shunt) {
        const double series_scale = std::max(series.real(), series.imag());
        const double shunt_scale = std::max(shunt.real(), shunt.imag());
        if (series_scale == 0. || shunt_scale == 0.) {
            return 0.;
        }
        int series_exponent, shunt_exponent;
        std::frexp(series_scale, &series_exponent);
        std::frexp(shunt_scale, &shunt_exponent);
        series = {std::scalbn(series.real(), -series_exponent),
                  std::scalbn(series.imag(), -series_exponent)};
        shunt = {std::scalbn(shunt.real(), -shunt_exponent),
                 std::scalbn(shunt.imag(), -shunt_exponent)};
        Complex product = series * shunt;
        int exponent = series_exponent + shunt_exponent;
        if (exponent % 2 != 0) {
            product *= 2.;
            --exponent;
        }
        // Nonnegative R,L,G,C put a and b in the first quadrant, so this
        // principal root has the same branch as sqrt(a)*sqrt(b).
        const Complex root = std::sqrt(product);
        return {std::scalbn(root.real(), exponent / 2),
                std::scalbn(root.imag(), exponent / 2)};
    }

public:
    RlgcTransmissionLineModel(std::string name,
                              RlgcPerLength coefficients,
                              double length_m,
                              double reference_ohms = 50.)
        : name_(std::move(name)), coefficients_(coefficients), length_(length_m),
          reference_(reference_ohms) {
        if (name_.empty() || !std::isfinite(reference_) || reference_ <= 0.) {
            throw std::invalid_argument("invalid RLGC name or reference impedance");
        }
        for (double value : {length_,
                             coefficients_.resistance_ohms_per_m,
                             coefficients_.inductance_h_per_m,
                             coefficients_.conductance_s_per_m,
                             coefficients_.capacitance_f_per_m}) {
            if (!std::isfinite(value) || value < 0.) {
                throw std::invalid_argument(
                    "RLGC coefficients and length must be finite nonnegative");
            }
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
            throw std::out_of_range("RLGC line port");
        }
        return {index, index == 0 ? "in" : "out", {reference_, 0.}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0.) {
            throw std::invalid_argument("invalid RLGC frequency");
        }
        if (length_ == 0.) {
            return {2, {0., 1., 1., 0.}};
        }
        constexpr double two_pi = 6.28318530717958647692;
        const auto &c = coefficients_;
        const Complex series = {
            normalized_product({c.resistance_ohms_per_m, length_}, reference_),
            normalized_product({frequency_hz, c.inductance_h_per_m, two_pi, length_}, reference_)};
        const Complex shunt = {
            normalized_product({c.conductance_s_per_m, length_, reference_}),
            normalized_product({frequency_hz, c.capacitance_f_per_m, two_pi, length_, reference_})};
        if (!finite(series) || !finite(shunt)) {
            throw std::overflow_error("RLGC normalization overflow");
        }
        const Complex x = electrical_length(series, shunt);
        if (!finite(x)) {
            throw std::overflow_error("RLGC normalization overflow");
        }
        const Complex propagation = std::exp(-x);
        Complex scaled_sinh;
        if (std::abs(x) < 1e-3) {
            const Complex square = x * x;
            // exp(-x)*sinh(x)/x, including the exact x=0 series/shunt limits.
            scaled_sinh = propagation *
                          (1. + square * (1. / 6. + square * (1. / 120. + square / 5040.)));
        } else {
            scaled_sinh = (0.5 * (1. - propagation * propagation)) / x;
        }
        // Scale before adding a and b. The bound |exp(-x)*sinh(x)/x| <= 1
        // for Re(x) >= 0 leaves headroom for every real/imaginary product and
        // sum. Avoid scaling small inputs, preserving subnormal reflections.
        const double largest_component =
            std::max({series.real(), series.imag(), shunt.real(), shunt.imag()});
        const double scale =
            largest_component > std::numeric_limits<double>::max() / 8. ? 0.25 : 1.;
        const Complex denominator =
            scale * (1. + propagation * propagation) +
            (scale * series + scale * shunt) * scaled_sinh;
        const Complex numerator = (scale * series - scale * shunt) * scaled_sinh;
        if (!finite(denominator) || !finite(numerator) || std::abs(denominator) == 0.) {
            throw std::overflow_error("RLGC scattering conversion outside numerical range");
        }
        const Complex reflection = numerator / denominator;
        const Complex transmission = (2. * scale) * propagation / denominator;
        if (!finite(reflection) || !finite(transmission)) {
            throw std::overflow_error("RLGC scattering overflow");
        }
        return {2, {reflection, transmission, transmission, reflection}};
    }
};
} // namespace rfmodel
