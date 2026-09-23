#pragma once
#include "power_wave_spectrum.hpp"
#include <limits>

namespace rfmodel {
// Prescribed LO; matched RF/IF ports, equal positive real reference resistance.
// y(t)=2*g*x(t)*cos(2*pi*lo_bin*spacing_hz*t+phase).
class IdealRealMixer final : public RFDeviceModel, public SpectrumTransmissionProvider {
    std::string name_;
    int lo_bin_;
    Complex pump_;
    double reference_;

public:
    IdealRealMixer(std::string name,
                   int lo_bin,
                   double conversion_gain_db,
                   double lo_phase_radians = 0.,
                   double reference_ohms = 50.)
        : name_(std::move(name)), lo_bin_(lo_bin), reference_(reference_ohms) {
        if (name_.empty() || lo_bin_ <= 0 || !std::isfinite(conversion_gain_db) ||
            !std::isfinite(lo_phase_radians) || !std::isfinite(reference_) || reference_ <= 0) {
            throw std::invalid_argument("invalid ideal mixer parameters");
        }
        const double gain = std::pow(10., conversion_gain_db / 20.);
        if (!std::isfinite(gain) || gain == 0.) {
            throw std::overflow_error("mixer gain range exceeded");
        }
        pump_ = std::polar(gain, lo_phase_radians);
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= 2) {
            throw std::out_of_range("mixer port");
        }
        return {index, index == 0 ? "RF" : "IF", {reference_, 0.}};
    }

    PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const override {
        validate_power_wave_spectrum(incident);
        if (!std::isfinite(lo_bin_ * incident.spacing_hz)) {
            throw std::overflow_error("LO frequency overflow");
        }
        const double root_two = std::sqrt(2.);
        std::map<int, Complex> coefficients;
        auto shift = [&](int bin, Complex value) {
            for (int side : {-1, 1}) {
                const auto destination =
                    static_cast<long long>(bin) + static_cast<long long>(side) * lo_bin_;
                if (destination > std::numeric_limits<int>::max() ||
                    destination < -static_cast<long long>(std::numeric_limits<int>::max())) {
                    throw std::overflow_error("mixer frequency index overflow");
                }
                coefficients[static_cast<int>(destination)] +=
                    value * (side == 1 ? pump_ : std::conj(pump_));
            }
        };
        for (const auto &entry : incident.amplitudes) {
            if (entry.second == Complex{}) {
                continue;
            }
            if (entry.first == 0) {
                shift(0, entry.second);
            } else {
                shift(entry.first, entry.second / root_two);
                shift(-entry.first, std::conj(entry.second) / root_two);
            }
        }
        PowerWaveSpectrum output{incident.spacing_hz, {}};
        for (const auto &entry : coefficients) {
            if (!std::isfinite(entry.second.real()) || !std::isfinite(entry.second.imag())) {
                throw std::overflow_error("mixer coefficient overflow");
            }
            if (entry.first < 0) {
                continue;
            }
            const Complex amplitude =
                entry.first == 0 ? Complex{entry.second.real(), 0.} : entry.second * root_two;
            if (!std::isfinite(amplitude.real()) || !std::isfinite(amplitude.imag()) ||
                !std::isfinite(entry.first * output.spacing_hz)) {
                throw std::overflow_error("mixer result overflow");
            }
            if (amplitude != Complex{}) {
                output.amplitudes[entry.first] = amplitude;
            }
        }
        if (output.amplitudes.size() > 2048) {
            throw std::length_error("mixer output exceeds spectrum size limit");
        }
        return output;
    }
};
} // namespace rfmodel
