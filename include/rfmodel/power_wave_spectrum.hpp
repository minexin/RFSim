#pragma once
#include "device_model.hpp"
#include <cmath>
#include <map>

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

class SpectrumTransmissionProvider {
public:
    virtual ~SpectrumTransmissionProvider() = default;
    virtual PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const = 0;
};

using NonlinearTransmissionProvider = SpectrumTransmissionProvider;
} // namespace rfmodel
