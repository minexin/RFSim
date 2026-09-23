#pragma once
#include "device_model.hpp"
#include <cmath>

namespace rfmodel {
// Ideal reciprocal, matched two-port with constant insertion loss and delay.
// Specializations: delay=0 gives an attenuator; loss=0 gives a lossless delay line.
class MatchedTransmissionModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    double loss_db_, delay_s_, reference_;

public:
    MatchedTransmissionModel(std::string name,
                             double loss_db,
                             double delay_s = 0,
                             double reference_ohms = 50)
        : name_(std::move(name)), loss_db_(loss_db), delay_s_(delay_s), reference_(reference_ohms) {
        if (name_.empty() || !std::isfinite(loss_db_) || loss_db_ < 0 || !std::isfinite(delay_s_) ||
            delay_s_ < 0 || !std::isfinite(reference_) || reference_ <= 0) {
            throw std::invalid_argument("invalid matched transmission parameters");
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
            throw std::out_of_range("transmission model port");
        }
        return {i, i == 0 ? "in" : "out", {reference_, 0}};
    }

    SMatrix s_parameters(double f) const override {
        if (!std::isfinite(f) || f < 0) {
            throw std::invalid_argument("invalid frequency");
        }
        const double cycles = f * delay_s_;
        if (!std::isfinite(cycles)) {
            throw std::overflow_error("delay phase overflow");
        }
        const double phase = -2 * 3.14159265358979323846 * std::remainder(cycles, 1.0);
        const Complex transmission = std::polar(std::pow(10., -loss_db_ / 20.), phase);
        return {2, {{}, transmission, transmission, {}}};
    }
};
} // namespace rfmodel
