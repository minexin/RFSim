#pragma once
#include "single_tone_amplifier.hpp"

namespace rfmodel {
// These families are not an implicitly summed output wave. The calibrated direct
// response already includes compression; generated carrier-bin products remain
// explicit to avoid silently double-counting or discarding them.
struct LimitedAmplifierResponse {
    PowerWaveSpectrum direct;
    PowerWaveSpectrum second_order;
    PowerWaveSpectrum third_order;
    double total_input_power_w{};
    double limited_input_power_w{};
};

class MultiToneLimitedAmplifier {
    SaturatingFundamentalCompression fundamental_;
    MatchedPolynomialAmplifier second_order_;
    MatchedPolynomialAmplifier third_order_;
    SoftInputLimiter limiter_;

    static void check_output(PowerWaveSpectrum &spectrum) {
        spectrum.amplitudes.erase(0);
        for (const auto &entry : spectrum.amplitudes) {
            if (!std::isfinite(std::norm(entry.second))) {
                throw std::overflow_error("multitone amplifier output power overflow");
            }
        }
    }

public:
    MultiToneLimitedAmplifier(double power_gain_db,
                              double output_p1db_dbm,
                              double output_saturation_dbm,
                              double input_ip2_dbm,
                              double input_ip3_dbm,
                              double reference_ohms = 50.)
        : fundamental_(power_gain_db, output_p1db_dbm, output_saturation_dbm),
          second_order_(MatchedPolynomialAmplifier::from_intercepts("second-order products",
                                                                    power_gain_db,
                                                                    input_ip2_dbm,
                                                                    input_ip3_dbm,
                                                                    reference_ohms)
                            .homogeneous_component(2)),
          third_order_(MatchedPolynomialAmplifier::from_intercepts("third-order products",
                                                                   power_gain_db,
                                                                   input_ip2_dbm,
                                                                   input_ip3_dbm,
                                                                   reference_ohms)
                           .homogeneous_component(3)),
          limiter_(std::pow(10., (output_p1db_dbm - power_gain_db - 34.) / 10.),
                   std::pow(10., (output_saturation_dbm - power_gain_db - 31.) / 10.)) {
    }

    LimitedAmplifierResponse evaluate(const PowerWaveSpectrum &incident) const {
        // Validates the grid, DC exclusion, finite individual powers and finite sum.
        const double total = incident_rf_power_watts({incident});
        LimitedAmplifierResponse result{{incident.spacing_hz, {}},
                                        {incident.spacing_hz, {}},
                                        {incident.spacing_hz, {}},
                                        total,
                                        0.};
        if (total == 0.) {
            return result;
        }
        const double drive = std::sqrt(total);
        const double limited_drive = limiter_.limit({drive, 0.}).real();
        result.limited_input_power_w = limited_drive * limited_drive;
        const double scale = limited_drive / drive;
        PowerWaveSpectrum limited{incident.spacing_hz, {}};
        for (const auto &entry : incident.amplitudes) {
            if (entry.second == Complex{}) {
                continue;
            }
            limited.amplitudes[entry.first] = entry.second * scale;
            result.direct.amplitudes[entry.first] =
                fundamental_.transmit_fundamental(entry.second, total);
        }
        result.second_order = second_order_.transmit(limited);
        result.third_order = third_order_.transmit(limited);
        check_output(result.direct);
        check_output(result.second_order);
        check_output(result.third_order);
        return result;
    }
};
} // namespace rfmodel
