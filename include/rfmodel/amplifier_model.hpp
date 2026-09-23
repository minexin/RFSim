#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
// Positive real reference impedance. Scaling avoids overflow in Z +/- Zref.
inline Complex reflection_from_impedance(Complex impedance, double reference_ohms = 50) {
    if (!std::isfinite(impedance.real()) || !std::isfinite(impedance.imag()) ||
        !std::isfinite(reference_ohms) || reference_ohms <= 0) {
        throw std::invalid_argument("invalid impedance or reference");
    }
    const double scale =
        std::max({std::abs(impedance.real()), std::abs(impedance.imag()), reference_ohms});
    const Complex normalized = impedance / scale;
    const double reference = reference_ohms / scale;
    const Complex denominator = normalized + reference;
    if (denominator == Complex{}) {
        throw std::domain_error("impedance gives singular reflection");
    }
    const Complex reflection = (normalized - reference) / denominator;
    if (!std::isfinite(reflection.real()) || !std::isfinite(reflection.imag())) {
        throw std::overflow_error("impedance reflection overflow");
    }
    return reflection;
}

struct LinearAmplifierParameters {
    double gain_db = 20;
    double gain_phase_degrees = 0;
    double reverse_isolation_db = 50;
    double reverse_phase_degrees = 0;
    Complex input_impedance_ohms = {50, 0};
    Complex output_impedance_ohms = {50, 0};
    double reference_ohms = 50;
};

// Frequency-independent bilateral small-signal building block. Gain specifies
// |S21|^2 at the reference planes, not operating gain under mismatched loads.
// Does not implement RFAMP compression, noise defaults, or its DC blocking.
class LinearAmplifierModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    double reference_;
    SMatrix scattering_;

    static Complex transmission(double gain_db, double phase_degrees) {
        if ((!std::isfinite(gain_db) && gain_db != -std::numeric_limits<double>::infinity()) ||
            !std::isfinite(phase_degrees)) {
            throw std::invalid_argument("invalid amplifier gain or phase");
        }
        const double magnitude = std::pow(10.0, gain_db / 20.0);
        if (!std::isfinite(magnitude)) {
            throw std::overflow_error("amplifier gain overflow");
        }
        const double phase =
            std::remainder(phase_degrees, 360.0) * (3.14159265358979323846 / 180.0);
        return std::polar(magnitude, phase);
    }

public:
    explicit LinearAmplifierModel(std::string name,
                                  const LinearAmplifierParameters &parameters = {})
        : name_(std::move(name)), reference_(parameters.reference_ohms), scattering_{2, {}} {
        if (name_.empty() || parameters.reverse_isolation_db < 0) {
            throw std::invalid_argument("invalid amplifier name or reverse isolation");
        }
        const auto input = reflection_from_impedance(parameters.input_impedance_ohms, reference_);
        const auto output = reflection_from_impedance(parameters.output_impedance_ohms, reference_);
        const auto forward = transmission(parameters.gain_db, parameters.gain_phase_degrees);
        const auto reverse =
            transmission(-parameters.reverse_isolation_db, parameters.reverse_phase_degrees);
        scattering_.values = {input, reverse, forward, output};
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= 2) {
            throw std::out_of_range("amplifier port");
        }
        return {index, index == 0 ? "in" : "out", {reference_, 0}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0) {
            throw std::invalid_argument("invalid amplifier frequency");
        }
        return scattering_;
    }
};
} // namespace rfmodel
