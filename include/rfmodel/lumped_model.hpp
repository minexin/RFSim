#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace rfmodel {
enum class LumpedConnection {
    SeriesImpedance,
    ShuntAdmittance
};

// Constitutive law returns ohms (series) or siemens (shunt), at frequency in Hz.
class LumpedTwoPortModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    LumpedConnection connection_;
    std::function<Complex(double)> law_;
    double reference_;

public:
    LumpedTwoPortModel(std::string name,
                       LumpedConnection connection,
                       std::function<Complex(double)> law,
                       double reference_ohms = 50.)
        : name_(std::move(name)), connection_(connection), law_(std::move(law)),
          reference_(reference_ohms) {
        if (name_.empty() || !law_ || !std::isfinite(reference_) || reference_ <= 0 ||
            (connection_ != LumpedConnection::SeriesImpedance &&
             connection_ != LumpedConnection::ShuntAdmittance)) {
            throw std::invalid_argument("invalid lumped model parameters");
        }
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t i) const override {
        if (i >= 2) {
            throw std::out_of_range("lumped model port");
        }
        return {i, i == 0 ? "in" : "out", {reference_, 0.}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0) {
            throw std::invalid_argument("invalid lumped frequency");
        }
        const auto value = law_(frequency_hz);
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
            throw std::invalid_argument("nonfinite lumped constitutive value");
        }
        const bool series = connection_ == LumpedConnection::SeriesImpedance;
        const Complex normalized = series ? value / reference_ : value * reference_;
        if (!std::isfinite(normalized.real()) || !std::isfinite(normalized.imag())) {
            throw std::overflow_error("lumped normalization overflow");
        }
        // Divide first to avoid overflow in 2+normalized for large finite values.
        const double scale =
            std::max(2., std::max(std::abs(normalized.real()), std::abs(normalized.imag())));
        const auto denominator = 2. / scale + normalized / scale;
        if (std::abs(denominator) <= 64 * std::numeric_limits<double>::epsilon()) {
            throw std::domain_error("singular lumped scattering conversion");
        }
        const Complex transmission = (2. / scale) / denominator;
        const Complex reflection = (series ? 1. : -1.) * (normalized / scale) / denominator;
        return {2, {reflection, transmission, transmission, reflection}};
    }
};
} // namespace rfmodel
