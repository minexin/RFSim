#pragma once
#include "ideal_devices.hpp"
#include <algorithm>
#include <limits>

namespace rfmodel {
// Uniform reciprocal line with real characteristic impedance. Loss is the
// one-way propagation loss, not the insertion loss of the mismatched two-port.
class TransmissionLineModel final : public RFDeviceModel, public SParameterProvider {
    MatchedTransmissionModel propagation_;
    double reflection_;

public:
    TransmissionLineModel(std::string name,
                          double characteristic_ohms,
                          double delay_s,
                          double propagation_loss_db = 0.,
                          double reference_ohms = 50.)
        : propagation_(std::move(name), propagation_loss_db, delay_s, reference_ohms) {
        if (!std::isfinite(characteristic_ohms) || characteristic_ohms <= 0.) {
            throw std::invalid_argument("invalid line characteristic impedance");
        }
        const double scale = std::max(characteristic_ohms, reference_ohms);
        const double line = characteristic_ohms / scale;
        const double reference = reference_ohms / scale;
        reflection_ = (line - reference) / (line + reference);
        if (1. - std::abs(reflection_) <= 64 * std::numeric_limits<double>::epsilon()) {
            throw std::invalid_argument("line impedance contrast exceeds numerical range");
        }
    }

    std::string name() const override {
        return propagation_.name();
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        return propagation_.port(index);
    }

    SMatrix s_parameters(double frequency_hz) const override {
        const Complex propagation = propagation_.s_parameters(frequency_hz)(1, 0);
        const Complex round_trip = propagation * propagation;
        const double coupling = (1. - reflection_) * (1. + reflection_);
        // This form preserves the coupling term at zero electrical length.
        const Complex denominator = coupling + reflection_ * reflection_ * (1. - round_trip);
        const Complex reflected = reflection_ * (1. - round_trip) / denominator;
        const Complex transmitted = coupling * propagation / denominator;
        return {2, {reflected, transmitted, transmitted, reflected}};
    }
};
} // namespace rfmodel
