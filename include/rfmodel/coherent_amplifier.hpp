#pragma once
#include "coherence.hpp"
#include "single_tone_amplifier.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace rfmodel {
struct CoherentAmplifierTerm {
    int order{};
    // Signed, one-based indices into CoherentAmplifierResponse::inputs.
    // Negative means conjugation; repetitions are retained, not cancelled.
    std::array<int, 3> input_indices{};
    CoherentComponent component;
};

struct CoherentAmplifierResponse {
    std::vector<CoherentComponent> inputs;
    std::vector<CoherentAmplifierTerm> terms;
    double total_input_power_w{};
    double limited_input_power_w{};
};

// Matched forward RFAMP approximation: shared compression and limited quadratic/
// cubic products. Source-kind inputs only; no recursive nonlinear lineage, noise,
// DC, AM/PM or reverse feedback. Distinct origins remain distinct output groups.
class CoherentLimitedAmplifier {
    SaturatingFundamentalCompression fundamental_;
    MatchedPolynomialAmplifier polynomial_;
    SoftInputLimiter limiter_;
    double reference_;

public:
    CoherentLimitedAmplifier(double power_gain_db,
                             double output_p1db_dbm,
                             double output_saturation_dbm,
                             double input_ip2_dbm,
                             double input_ip3_dbm,
                             double reference_ohms = 50.)
        : fundamental_(power_gain_db, output_p1db_dbm, output_saturation_dbm),
          polynomial_(MatchedPolynomialAmplifier::from_intercepts(
              "coherent products", power_gain_db, input_ip2_dbm, input_ip3_dbm, reference_ohms)),
          limiter_(std::pow(10., (output_p1db_dbm - power_gain_db - 34.) / 10.),
                   std::pow(10., (output_saturation_dbm - power_gain_db - 31.) / 10.)),
          reference_(reference_ohms) {
    }

    CoherentAmplifierResponse evaluate(double spacing_hz,
                                       const std::vector<CoherentComponent> &input,
                                       std::uint64_t reserved_group_max = 0) const {
        const auto reduced = reduce_coherent_components(spacing_hz, input);
        CoherentAmplifierResponse result{reduced.components, {}, reduced.total_power_w, 0.};
        std::uint64_t highest_group = reserved_group_max;
        std::vector<int> signed_inputs;
        for (std::size_t i = 0; i < result.inputs.size(); ++i) {
            const auto &c = result.inputs[i];
            if (c.kind != SpectrumKind::source) {
                throw std::invalid_argument("coherent amplifier requires source-kind inputs");
            }
            highest_group = std::max(highest_group, c.coherence_group);
            if (c.amplitude != Complex{}) {
                if (signed_inputs.size() >= 128) {
                    throw std::length_error("coherent amplifier accepts at most 64 active groups");
                }
                signed_inputs.push_back(-static_cast<int>(i + 1));
                signed_inputs.push_back(static_cast<int>(i + 1));
            }
            auto direct = c;
            direct.amplitude =
                fundamental_.transmit_fundamental(c.amplitude, reduced.total_power_w);
            result.terms.push_back({1, {static_cast<int>(i + 1), 0, 0}, direct});
        }
        if (reduced.total_power_w == 0.) {
            return result;
        }
        const double drive = std::sqrt(reduced.total_power_w);
        const double limited = limiter_.limit({drive, 0.}).real();
        result.limited_input_power_w = limited * limited;
        const double scale = limited / drive;
        const double voltage_scale = std::sqrt(reference_) / std::sqrt(2.);
        const double quadratic = polynomial_.voltage_coefficient(2) * voltage_scale;
        const double cubic = polynomial_.voltage_coefficient(3) * voltage_scale * voltage_scale;
        std::sort(signed_inputs.begin(), signed_inputs.end());

        auto append = [&](int order, std::array<int, 3> indices, double coefficient) {
            long long bin = 0;
            double bandwidth = 0.;
            Complex amplitude{coefficient, 0.};
            for (int i = 0; i < order; ++i) {
                const int index = indices[i];
                const auto &c = result.inputs[static_cast<std::size_t>(std::abs(index) - 1)];
                bin += index < 0 ? -static_cast<long long>(c.bin) : c.bin;
                bandwidth += c.bandwidth_hz;
                amplitude *= (index < 0 ? std::conj(c.amplitude) : c.amplitude) * scale;
            }
            if (std::abs(bin) > std::numeric_limits<int>::max()) {
                throw std::overflow_error("coherent amplifier frequency overflow");
            }
            if (bin <= 0 || amplitude == Complex{}) {
                return;
            }
            if (result.terms.size() >= 4096) {
                throw std::length_error("coherent amplifier exceeds 4096 output terms");
            }
            const bool harmonic = indices[0] > 0 && indices[0] == indices[order - 1];
            result.terms.push_back({order,
                                    indices,
                                    {static_cast<int>(bin),
                                     harmonic ? SpectrumKind::harmonic : SpectrumKind::intermod,
                                     bandwidth,
                                     0,
                                     amplitude}});
        };
        for (std::size_t i = 0; i < signed_inputs.size(); ++i) {
            for (std::size_t j = i; j < signed_inputs.size(); ++j) {
                append(2, {signed_inputs[i], signed_inputs[j], 0}, quadratic * (i == j ? 1. : 2.));
                for (std::size_t k = j; k < signed_inputs.size(); ++k) {
                    const double multiplicity = i == k ? 1. : (i == j || j == k ? 3. : 6.);
                    append(3,
                           {signed_inputs[i], signed_inputs[j], signed_inputs[k]},
                           cubic * multiplicity);
                }
            }
        }
        std::sort(result.terms.begin(), result.terms.end(), [](const auto &a, const auto &b) {
            return std::tie(a.order, a.component.bin, a.input_indices) <
                   std::tie(b.order, b.component.bin, b.input_indices);
        });
        std::vector<CoherentComponent> output;
        for (auto &term : result.terms) {
            if (term.order > 1) {
                if (highest_group == std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("coherent amplifier group IDs exhausted");
                }
                term.component.coherence_group = ++highest_group;
            }
            output.push_back(term.component);
        }
        // Validate generated bands, powers and their sum before publishing anything.
        reduce_coherent_components(spacing_hz, output);
        return result;
    }
};
} // namespace rfmodel
