#pragma once
#include "device_model.hpp"
#include <cmath>

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
        const Complex series =
            Complex{c.resistance_ohms_per_m, frequency_hz * c.inductance_h_per_m * two_pi} *
            (length_ / reference_);
        const Complex shunt =
            Complex{c.conductance_s_per_m, frequency_hz * c.capacitance_f_per_m * two_pi} *
            length_ * reference_;
        // Separate square roots avoid forming the potentially overflowing Z*Y product.
        const Complex electrical_length = std::sqrt(series) * std::sqrt(shunt);
        if (!finite(series) || !finite(shunt) || !finite(electrical_length)) {
            throw std::overflow_error("RLGC normalization overflow");
        }
        const Complex propagation = std::exp(-electrical_length);
        Complex scaled_sinh;
        if (std::abs(electrical_length) < 1e-3) {
            const Complex square = electrical_length * electrical_length;
            // 2*exp(-x)*sinh(x)/x, including the exact x=0 series/shunt limits.
            scaled_sinh = 2. * propagation *
                          (1. + square * (1. / 6. + square * (1. / 120. + square / 5040.)));
        } else {
            scaled_sinh = (1. - propagation * propagation) / electrical_length;
        }
        const Complex denominator =
            2. * (1. + propagation * propagation) + (series + shunt) * scaled_sinh;
        const Complex numerator = (series - shunt) * scaled_sinh;
        if (!finite(denominator) || !finite(numerator) || std::abs(denominator) == 0.) {
            throw std::overflow_error("RLGC scattering conversion outside numerical range");
        }
        const Complex reflection = numerator / denominator;
        const Complex transmission = 4. * propagation / denominator;
        if (!finite(reflection) || !finite(transmission)) {
            throw std::overflow_error("RLGC scattering overflow");
        }
        return {2, {reflection, transmission, transmission, reflection}};
    }
};
} // namespace rfmodel
