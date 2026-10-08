#pragma once
#include "single_tone_amplifier.hpp"
#include <algorithm>
#include <array>
#include <tuple>

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

struct AmplifierMixingTerm {
    int order{};
    int bin{};
    // First order entries are a single positive bin. Generated entries contain
    // sorted signed input bins; negative means conjugation, repeats mean powers.
    // Only the first 'order' slots are used. Unused slots are zero.
    std::array<int, 3> contributors{};
    Complex amplitude{};
};

struct TracedAmplifierResponse {
    std::vector<AmplifierMixingTerm> terms;
    double total_input_power_w{};
    double limited_input_power_w{};
};

class MultiToneLimitedAmplifier {
    SaturatingFundamentalCompression fundamental_;
    MatchedPolynomialAmplifier second_order_;
    MatchedPolynomialAmplifier third_order_;
    SoftInputLimiter limiter_;
    double reference_;

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
                   std::pow(10., (output_saturation_dbm - power_gain_db - 31.) / 10.)),
          reference_(reference_ohms) {
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

    // Local generating-input provenance, not a complete multi-stage source history.
    // Work and term caps reject the entire operation; no truncation or aliasing.
    TracedAmplifierResponse evaluate_terms(const PowerWaveSpectrum &incident) const {
        const auto response = evaluate(incident);
        TracedAmplifierResponse traced{
            {}, response.total_input_power_w, response.limited_input_power_w};
        if (response.total_input_power_w == 0.) {
            return traced;
        }
        for (const auto &entry : response.direct.amplitudes) {
            traced.terms.push_back({1, entry.first, {entry.first, 0, 0}, entry.second});
        }
        const double scale =
            std::sqrt(response.limited_input_power_w) / std::sqrt(response.total_input_power_w);
        std::map<int, Complex> signed_input;
        for (const auto &entry : incident.amplitudes) {
            if (entry.second != Complex{}) {
                signed_input[entry.first] = entry.second * scale;
                signed_input[-entry.first] = std::conj(entry.second) * scale;
            }
        }
        const std::vector<std::pair<int, Complex>> sources(signed_input.begin(),
                                                           signed_input.end());
        const double voltage_scale = std::sqrt(reference_) / std::sqrt(2.);
        const double quadratic = second_order_.voltage_coefficient(2) * voltage_scale;
        const double cubic = third_order_.voltage_coefficient(3) * voltage_scale * voltage_scale;
        std::size_t work = 0;
        auto append = [&](int order, std::array<int, 3> contributors, Complex amplitude) {
            if (++work > 10000000) {
                throw std::length_error("mixing provenance work limit exceeded");
            }
            long long bin = 0;
            for (int index = 0; index < order; ++index) {
                bin += contributors[index];
            }
            if (bin > std::numeric_limits<int>::max() ||
                bin < -static_cast<long long>(std::numeric_limits<int>::max())) {
                throw std::overflow_error("mixing provenance frequency overflow");
            }
            if (bin <= 0 || amplitude == Complex{}) {
                return;
            }
            if (!std::isfinite(bin * incident.spacing_hz) || !std::isfinite(amplitude.real()) ||
                !std::isfinite(amplitude.imag()) || !std::isfinite(std::norm(amplitude))) {
                throw std::overflow_error("mixing provenance output overflow");
            }
            if (traced.terms.size() >= 4096) {
                throw std::length_error("mixing provenance term limit exceeded");
            }
            traced.terms.push_back({order, static_cast<int>(bin), contributors, amplitude});
        };
        for (std::size_t i = 0; i < sources.size(); ++i) {
            for (std::size_t j = i; j < sources.size(); ++j) {
                append(2,
                       {sources[i].first, sources[j].first, 0},
                       quadratic * sources[i].second * sources[j].second * (i == j ? 1. : 2.));
                for (std::size_t k = j; k < sources.size(); ++k) {
                    const double multiplicity = i == k ? 1. : (i == j || j == k ? 3. : 6.);
                    append(3,
                           {sources[i].first, sources[j].first, sources[k].first},
                           cubic * sources[i].second * sources[j].second * sources[k].second *
                               multiplicity);
                }
            }
        }
        std::sort(traced.terms.begin(), traced.terms.end(), [](const auto &a, const auto &b) {
            return std::tie(a.order, a.bin, a.contributors) <
                   std::tie(b.order, b.bin, b.contributors);
        });
        return traced;
    }
};
} // namespace rfmodel
