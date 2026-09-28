#pragma once
#include "device_model.hpp"
#include <cmath>

namespace rfmodel {
namespace multiport_detail {
class ConstantModel : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    double reference_;

protected:
    SMatrix scattering_;

    ConstantModel(std::string name, double reference_ohms)
        : name_(std::move(name)), reference_(reference_ohms) {
        if (name_.empty() || !std::isfinite(reference_) || reference_ <= 0.) {
            throw std::invalid_argument("invalid multiport name or reference impedance");
        }
    }

public:
    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return scattering_.ports;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= port_count()) {
            throw std::out_of_range("multiport device port");
        }
        return {index, "port" + std::to_string(index), {reference_, 0.}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0.) {
            throw std::invalid_argument("invalid multiport frequency");
        }
        return scattering_;
    }
};
} // namespace multiport_detail

// Port 0 is common; ports 1..N are matched, mutually isolated branches.
// The isolation network dissipates differential-mode inputs even at zero excess loss.
class EqualPowerDividerModel final : public multiport_detail::ConstantModel {
public:
    EqualPowerDividerModel(std::string name,
                           std::size_t branches,
                           double excess_loss_db = 0.,
                           double reference_ohms = 50.)
        : ConstantModel(std::move(name), reference_ohms) {
        if (branches < 2 || branches > 64 || !std::isfinite(excess_loss_db) ||
            excess_loss_db < 0.) {
            throw std::invalid_argument("invalid divider branch count or loss");
        }
        const auto ports = branches + 1;
        scattering_ = {ports, std::vector<Complex>(ports * ports)};
        const double amplitude = std::pow(10., -excess_loss_db / 20.) / std::sqrt(double(branches));
        for (std::size_t branch = 1; branch < ports; ++branch) {
            scattering_(branch, 0) = amplitude;
            scattering_(0, branch) = amplitude;
        }
    }
};

// Exciting port 0 gives real through at 1, +j coupled at 2, and isolation at 3.
class QuadratureCouplerModel final : public multiport_detail::ConstantModel {
public:
    QuadratureCouplerModel(std::string name,
                           double coupled_power_fraction,
                           double excess_loss_db = 0.,
                           double reference_ohms = 50.)
        : ConstantModel(std::move(name), reference_ohms) {
        if (!std::isfinite(coupled_power_fraction) || coupled_power_fraction < 0. ||
            coupled_power_fraction > 1. || !std::isfinite(excess_loss_db) || excess_loss_db < 0.) {
            throw std::invalid_argument("invalid coupler power fraction or loss");
        }
        const double attenuation = std::pow(10., -excess_loss_db / 20.);
        const double through = attenuation * std::sqrt(1. - coupled_power_fraction);
        const Complex coupled{0., attenuation * std::sqrt(coupled_power_fraction)};
        scattering_ = {4,
                       {0.,
                        through,
                        coupled,
                        0.,
                        through,
                        0.,
                        0.,
                        coupled,
                        coupled,
                        0.,
                        0.,
                        through,
                        0.,
                        coupled,
                        through,
                        0.}};
    }
};
} // namespace rfmodel
