#pragma once
#include "interpolation.hpp"

namespace rfmodel {
// Owns its validated dataset; neither source-file changes nor caller mutation affect it.
class TabulatedSParameterModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    TouchstoneData data_;
    OutOfBand policy_;

public:
    TabulatedSParameterModel(std::string name,
                             TouchstoneData data,
                             OutOfBand policy = OutOfBand::Reject)
        : name_(std::move(name)), data_(std::move(data)), policy_(policy) {
        if (name_.empty()) {
            throw std::invalid_argument("empty model name");
        }
        validate_s_data(data_);
    }

    static TabulatedSParameterModel from_touchstone(std::string name,
                                                    const std::string &path,
                                                    OutOfBand policy = OutOfBand::Reject) {
        return TabulatedSParameterModel(std::move(name), read_touchstone(path), policy);
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return data_.ports;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= data_.ports) {
            throw std::out_of_range("model port index");
        }
        return {index, "Port" + std::to_string(index + 1), {data_.reference_impedance_ohms, 0}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        return interpolate_s(data_, frequency_hz, policy_);
    }

    double minimum_frequency_hz() const {
        return data_.frequencies_hz.front();
    }

    double maximum_frequency_hz() const {
        return data_.frequencies_hz.back();
    }
};
} // namespace rfmodel
