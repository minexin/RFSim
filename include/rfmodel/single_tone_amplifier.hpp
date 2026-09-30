#pragma once
#include "polynomial_amplifier.hpp"
#include "saturating_fundamental.hpp"

namespace rfmodel {
// Phase-preserving soft limiter for RMS waves. Both anchors are input powers in W.
class SoftInputLimiter {
    double knee_;
    double headroom_;

public:
    SoftInputLimiter(double knee_power_w, double limiting_power_w) {
        if (!std::isfinite(knee_power_w) || !std::isfinite(limiting_power_w) || knee_power_w <= 0 ||
            limiting_power_w <= knee_power_w) {
            throw std::invalid_argument("invalid soft limiter power anchors");
        }
        knee_ = std::sqrt(knee_power_w);
        headroom_ = std::sqrt(limiting_power_w) - knee_;
        if (headroom_ <= 0) {
            throw std::invalid_argument("limiter headroom is not representable");
        }
    }

    Complex limit(Complex incident) const {
        const double power = std::norm(incident);
        if (!std::isfinite(incident.real()) || !std::isfinite(incident.imag()) ||
            !std::isfinite(power)) {
            throw std::invalid_argument("nonfinite limiter incident power");
        }
        const double amplitude = std::sqrt(power);
        if (amplitude <= knee_) {
            return incident;
        }
        const double limited = knee_ + headroom_ * std::tanh((amplitude - knee_) / headroom_);
        return (incident / amplitude) * limited;
    }
};

// Matched single-tone response: independent fundamental, limited H2/H3, blocked DC.
// The -4/-1 dB limiter offsets are empirically identified, not a published vendor formula.
class SingleToneLimitedAmplifier final : public SpectrumTransmissionProvider {
    SaturatingFundamentalCompression fundamental_;
    MatchedPolynomialAmplifier harmonics_;
    SoftInputLimiter limiter_;

public:
    SingleToneLimitedAmplifier(double power_gain_db,
                               double output_p1db_dbm,
                               double output_saturation_dbm,
                               double input_ip2_dbm,
                               double input_ip3_dbm,
                               double reference_ohms = 50.)
        : fundamental_(power_gain_db, output_p1db_dbm, output_saturation_dbm),
          harmonics_(MatchedPolynomialAmplifier::from_intercepts(
              "limited harmonics", power_gain_db, input_ip2_dbm, input_ip3_dbm, reference_ohms)),
          limiter_(std::pow(10., (output_p1db_dbm - power_gain_db - 4. - 30.) / 10.),
                   std::pow(10., (output_saturation_dbm - power_gain_db - 1. - 30.) / 10.)) {
    }

    PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const override {
        validate_power_wave_spectrum(incident);
        int fundamental_bin = 0;
        Complex wave{};
        for (const auto &entry : incident.amplitudes) {
            if (entry.second == Complex{}) {
                continue;
            }
            if (entry.first == 0 || fundamental_bin != 0) {
                throw std::invalid_argument("single-tone amplifier requires one RF tone and no DC");
            }
            fundamental_bin = entry.first;
            wave = entry.second;
        }
        if (fundamental_bin == 0) {
            return {incident.spacing_hz, {}};
        }
        auto output =
            harmonics_.transmit({incident.spacing_hz, {{fundamental_bin, limiter_.limit(wave)}}});
        output.amplitudes.erase(0);
        output.amplitudes[fundamental_bin] = fundamental_.transmit_fundamental(wave);
        for (const auto &entry : output.amplitudes) {
            if (!std::isfinite(std::norm(entry.second))) {
                throw std::overflow_error("single-tone output power overflow");
            }
        }
        return output;
    }
};
} // namespace rfmodel
