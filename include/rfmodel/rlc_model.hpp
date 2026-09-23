#pragma once
#include "lumped_model.hpp"

namespace rfmodel {
enum class IdealElement {
    Resistor,
    Inductor,
    Capacitor
};

// Ideal, positive-valued R/L/C; value units are ohms/henries/farads.
class IdealRLCModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    IdealElement element_;
    LumpedConnection connection_;
    double value_, reference_;

public:
    IdealRLCModel(std::string name,
                  IdealElement element,
                  LumpedConnection connection,
                  double value,
                  double reference_ohms = 50.)
        : name_(std::move(name)), element_(element), connection_(connection), value_(value),
          reference_(reference_ohms) {
        if (name_.empty() || !std::isfinite(value_) || value_ <= 0 || !std::isfinite(reference_) ||
            reference_ <= 0 ||
            (element_ != IdealElement::Resistor && element_ != IdealElement::Inductor &&
             element_ != IdealElement::Capacitor) ||
            (connection_ != LumpedConnection::SeriesImpedance &&
             connection_ != LumpedConnection::ShuntAdmittance)) {
            throw std::invalid_argument("invalid ideal RLC parameters");
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
            throw std::out_of_range("RLC port");
        }
        return {i, i == 0 ? "in" : "out", {reference_, 0.}};
    }

    SMatrix s_parameters(double f) const override {
        if (!std::isfinite(f) || f < 0) {
            throw std::invalid_argument("invalid RLC frequency");
        }
        const bool series = connection_ == LumpedConnection::SeriesImpedance;
        const double sign = series ? 1. : -1.;
        if (f == 0 && element_ != IdealElement::Resistor) {
            const bool blocked = series ? (element_ == IdealElement::Capacitor)
                                        : (element_ == IdealElement::Inductor);
            const Complex r = blocked ? sign : 0., t = blocked ? 0. : 1.;
            return {2, {r, t, t, r}};
        }
        // Compute log(|Z/Z0|), then invert for a shunt element. This avoids
        // overflowing omega*L or underflowing omega*C before normalization.
        double log_magnitude = std::log(value_) - std::log(reference_);
        Complex phase = 1.;
        if (element_ != IdealElement::Resistor) {
            const double log_omega = std::log(2 * 3.14159265358979323846) + std::log(f);
            if (element_ == IdealElement::Inductor) {
                log_magnitude += log_omega;
                phase = {0., 1.};
            } else {
                log_magnitude = -log_omega - std::log(value_) - std::log(reference_);
                phase = {0., -1.};
            }
        }
        if (!series) {
            log_magnitude = -log_magnitude;
            phase = std::conj(phase);
        }
        log_magnitude -= std::log(2.);
        Complex t, r;
        if (log_magnitude > 0) {
            const double inverse = std::exp(-log_magnitude);
            t = inverse / (inverse + phase);
            r = sign * phase / (inverse + phase);
        } else {
            const auto q = std::exp(log_magnitude) * phase;
            t = 1. / (1. + q);
            r = sign * q / (1. + q);
        }
        return {2, {r, t, t, r}};
    }
};
} // namespace rfmodel
