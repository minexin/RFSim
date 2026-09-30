#pragma once
#include "device_model.hpp"
#include <cmath>
#include <map>
#include <vector>

namespace rfmodel {
// RMS power waves in sqrt(W); bin zero is real DC, negative bins are conjugates.
struct PowerWaveSpectrum {
    double spacing_hz{};
    std::map<int, Complex> amplitudes;
};

inline void validate_power_wave_spectrum(const PowerWaveSpectrum &spectrum) {
    if (!std::isfinite(spectrum.spacing_hz) || spectrum.spacing_hz <= 0 ||
        spectrum.amplitudes.size() > 2048) {
        throw std::invalid_argument("invalid power wave grid or size");
    }
    for (const auto &entry : spectrum.amplitudes) {
        if (entry.first < 0 || !std::isfinite(entry.first * spectrum.spacing_hz) ||
            !std::isfinite(entry.second.real()) || !std::isfinite(entry.second.imag()) ||
            (entry.first == 0 && entry.second.imag() != 0.)) {
            throw std::invalid_argument("invalid power wave coefficient");
        }
    }
}

// Sum incident powers across physical ports. Coherent paths at the same port/bin
// must already have been combined into that bin's amplitude by the wave solver.
inline double incident_rf_power_watts(const std::vector<PowerWaveSpectrum> &ports) {
    if (ports.empty() || ports.size() > 1024) {
        throw std::invalid_argument("expected 1..1024 incident port spectra");
    }
    double total = 0.;
    double correction = 0.;
    for (const auto &port : ports) {
        validate_power_wave_spectrum(port);
        for (const auto &entry : port.amplitudes) {
            if (entry.first == 0 && entry.second != Complex{}) {
                throw std::invalid_argument("RF drive requires explicit removal of DC");
            }
            const double power = std::norm(entry.second);
            const double adjusted = power - correction;
            const double next = total + adjusted;
            if (!std::isfinite(power) || !std::isfinite(next)) {
                throw std::overflow_error("incident RF power overflow");
            }
            correction = (next - total) - adjusted;
            total = next;
        }
    }
    return total;
}

class SpectrumTransmissionProvider {
public:
    virtual ~SpectrumTransmissionProvider() = default;
    virtual PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const = 0;
};

using NonlinearTransmissionProvider = SpectrumTransmissionProvider;
} // namespace rfmodel
