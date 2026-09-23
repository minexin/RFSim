#pragma once
#include "network.hpp"
#include "power_wave_spectrum.hpp"
#include <functional>

namespace rfmodel {
inline double spectrum_power_watts(const PowerWaveSpectrum &spectrum) {
    validate_power_wave_spectrum(spectrum);
    double power = 0.;
    for (const auto &entry : spectrum.amplitudes) {
        power += std::norm(entry.second);
    }
    if (!std::isfinite(power)) {
        throw std::overflow_error("spectrum total power overflow");
    }
    return power;
}

// Matched external source/load; internal linear reflections remain in the solve.
inline PowerWaveSpectrum
transmit_linear_spectrum(const PowerWaveSpectrum &input,
                         const std::vector<std::size_t> &external_ports,
                         double reference_ohms,
                         const std::function<LinearNetwork(double)> &build) {
    validate_power_wave_spectrum(input);
    if (!build || external_ports.size() != 2 || !std::isfinite(reference_ohms) ||
        reference_ohms <= 0) {
        throw std::invalid_argument("invalid linear spectrum request");
    }
    PowerWaveSpectrum output{input.spacing_hz, {}};
    for (const auto &entry : input.amplitudes) {
        const double frequency = entry.first * input.spacing_hz;
        try {
            const auto network = build(frequency);
            if (network.reference_impedance_ohms() != reference_ohms) {
                throw std::invalid_argument("linear spectrum reference differs");
            }
            const auto scattering = network.external_s(external_ports);
            auto amplitude = scattering(1, 0) * entry.second;
            if (!std::isfinite(amplitude.real()) || !std::isfinite(amplitude.imag())) {
                throw std::overflow_error("linear spectrum amplitude overflow");
            }
            if (entry.first == 0) {
                const double tolerance =
                    64 * std::numeric_limits<double>::epsilon() * std::abs(amplitude);
                if (std::abs(amplitude.imag()) > tolerance) {
                    throw std::invalid_argument(
                        "complex DC output cannot represent a real waveform");
                }
                amplitude = {amplitude.real(), 0.};
            }
            if (amplitude != Complex{}) {
                output.amplitudes[entry.first] = amplitude;
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("linear spectrum failed at " + std::to_string(frequency) +
                                     " Hz: " + error.what());
        }
    }
    validate_power_wave_spectrum(output);
    return output;
}

struct SpectrumStage {
    std::string name;
    double input_reference_ohms{50.};
    double output_reference_ohms{50.};
    std::function<PowerWaveSpectrum(const PowerWaveSpectrum &)> evaluate;
};

struct SpectrumStageResult {
    std::string name;
    double reference_ohms{};
    PowerWaveSpectrum output;
    double total_power_watts{};
};

struct SpectrumAnalysisResult {
    double input_power_watts{};
    std::vector<SpectrumStageResult> stages;
};

inline SpectrumAnalysisResult analyze_spectrum(const PowerWaveSpectrum &input,
                                               double source_reference_ohms,
                                               const std::vector<SpectrumStage> &stages) {
    if (!std::isfinite(source_reference_ohms) || source_reference_ohms <= 0) {
        throw std::invalid_argument("invalid source reference");
    }
    double reference = source_reference_ohms;
    // Validate all stage contracts before executing any callback.
    for (const auto &stage : stages) {
        if (stage.name.empty() || !stage.evaluate || stage.input_reference_ohms != reference ||
            !std::isfinite(stage.output_reference_ohms) || stage.output_reference_ohms <= 0) {
            throw std::invalid_argument("invalid spectrum stage contract: " + stage.name);
        }
        reference = stage.output_reference_ohms;
    }
    SpectrumAnalysisResult result{spectrum_power_watts(input), {}};
    auto current = input;
    for (const auto &stage : stages) {
        try {
            auto next = stage.evaluate(current);
            if (next.spacing_hz != input.spacing_hz) {
                throw std::invalid_argument("stage changed the common frequency grid");
            }
            const double power = spectrum_power_watts(next);
            result.stages.push_back({stage.name, stage.output_reference_ohms, next, power});
            current = std::move(next);
        } catch (const std::exception &error) {
            throw std::runtime_error("spectrum stage '" + stage.name + "': " + error.what());
        }
    }
    return result;
}
} // namespace rfmodel
