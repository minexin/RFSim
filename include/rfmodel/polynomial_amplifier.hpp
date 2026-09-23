#pragma once
#include "nonlinear_spectrum.hpp"

namespace rfmodel {
// Positive-frequency RMS power waves in sqrt(W); norm(coefficient) is tone power.
// Bin zero is a real DC power amplitude. Negative bins are implicit conjugates.
struct PowerWaveSpectrum {
    double spacing_hz{};
    std::map<int, Complex> amplitudes;
};

class NonlinearTransmissionProvider {
public:
    virtual ~NonlinearTransmissionProvider() = default;
    virtual PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const = 0;
};

class MatchedPolynomialAmplifier final : public RFDeviceModel,
                                         public SParameterProvider,
                                         public NonlinearTransmissionProvider {
    std::string name_;
    MemorylessPolynomial polynomial_;
    double reference_;
    double linear_gain_;

public:
    MatchedPolynomialAmplifier(std::string name,
                               std::vector<double> voltage_coefficients,
                               double reference_ohms = 50.)
        : name_(std::move(name)), polynomial_(voltage_coefficients), reference_(reference_ohms),
          linear_gain_(voltage_coefficients.size() > 1 ? voltage_coefficients[1] : 0.) {
        if (name_.empty() || !std::isfinite(reference_) || reference_ <= 0) {
            throw std::invalid_argument("invalid polynomial amplifier identity/reference");
        }
    }

    // Equal-tone extrapolated input IP3, with negative cubic compression.
    static MatchedPolynomialAmplifier from_iip3(std::string name,
                                                double power_gain_db,
                                                double input_ip3_dbm,
                                                double reference_ohms = 50.) {
        if (!std::isfinite(power_gain_db) || !std::isfinite(input_ip3_dbm) ||
            !std::isfinite(reference_ohms) || reference_ohms <= 0) {
            throw std::invalid_argument("invalid cubic amplifier parameters");
        }
        const double gain = std::pow(10., power_gain_db / 20.);
        const double intercept_watts = std::pow(10., (input_ip3_dbm - 30.) / 10.);
        const double cubic = -(2. / 3.) * (gain / reference_ohms) / intercept_watts;
        if (!std::isfinite(gain) || gain <= 0 || !std::isfinite(intercept_watts) ||
            intercept_watts <= 0 || !std::isfinite(cubic) || cubic == 0.) {
            throw std::overflow_error("cubic amplifier coefficient range exceeded");
        }
        return MatchedPolynomialAmplifier(std::move(name), {0., gain, 0., cubic}, reference_ohms);
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
        return {index, index == 0 ? "in" : "out", {reference_, 0.}};
    }

    // Zero-input operating point; no reflection or reverse transmission.
    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0) {
            throw std::invalid_argument("invalid amplifier frequency");
        }
        return {2, {0., 0., linear_gain_, 0.}};
    }

    PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const override {
        const double root_reference = std::sqrt(reference_);
        const double root_two = std::sqrt(2.);
        RealVoltageSpectrum voltage{incident.spacing_hz, {}};
        for (const auto &entry : incident.amplitudes) {
            const double factor = entry.first == 0 ? root_reference : root_reference / root_two;
            voltage.positive_frequency_coefficients[entry.first] = entry.second * factor;
        }
        const auto response = polynomial_.evaluate(voltage);
        PowerWaveSpectrum outgoing{response.spacing_hz, {}};
        for (const auto &entry : response.positive_frequency_coefficients) {
            const double factor =
                entry.first == 0 ? 1. / root_reference : root_two / root_reference;
            const auto amplitude = entry.second * factor;
            if (!std::isfinite(amplitude.real()) || !std::isfinite(amplitude.imag())) {
                throw std::overflow_error("amplifier output wave overflow");
            }
            outgoing.amplitudes[entry.first] = amplitude;
        }
        return outgoing;
    }
};
} // namespace rfmodel
