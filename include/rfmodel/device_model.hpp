#pragma once
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace rfmodel {
using Complex = std::complex<double>;

struct PortInfo {
    std::size_t id{};
    std::string name;
    Complex reference_impedance{50.0, 0.0};
};

struct FrequencyGrid {
    std::vector<double> hz;
};

// Row-major: row is outgoing port, column is incident port.
struct SMatrix {
    std::size_t ports{};
    std::vector<Complex> values;

    std::size_t index(std::size_t row, std::size_t column) const {
        if (row >= ports || column >= ports) {
            throw std::out_of_range("S matrix port index");
        }
        return row * ports + column;
    }

    Complex &operator()(std::size_t row, std::size_t column) {
        return values.at(index(row, column));
    }

    const Complex &operator()(std::size_t row, std::size_t column) const {
        return values.at(index(row, column));
    }
};

class RFDeviceModel {
public:
    virtual ~RFDeviceModel() = default;
    virtual std::string name() const = 0;
    virtual std::size_t port_count() const = 0;
    virtual PortInfo port(std::size_t) const = 0;
};

class SParameterProvider {
public:
    virtual ~SParameterProvider() = default;
    virtual SMatrix s_parameters(double frequency_hz) const = 0;
};

class StaticSParameterModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    std::vector<PortInfo> ports_;
    SMatrix s_;

public:
    StaticSParameterModel(std::string n, std::vector<PortInfo> p, SMatrix s)
        : name_(std::move(n)), ports_(std::move(p)), s_(std::move(s)) {
        if (s_.ports != ports_.size() || s_.values.size() != s_.ports * s_.ports) {
            throw std::invalid_argument("S matrix dimensions");
        }
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return ports_.size();
    }

    PortInfo port(std::size_t i) const override {
        return ports_.at(i);
    }

    SMatrix s_parameters(double) const override {
        return s_;
    }
};
} // namespace rfmodel
