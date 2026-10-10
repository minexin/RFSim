#pragma once
#include "coherent_amplifier.hpp"
#include "coherent_polynomial.hpp"

namespace rfmodel {
struct CoherentHighOrderAmplifierResponse {
    std::vector<CoherentComponent> inputs;
    std::vector<CoherentPolynomialTerm> terms;
    double total_input_power_w{};
    AmplifierOperatingPoint operating_point;
};

// Explicit a2..a11 voltage coefficients, shared compression and input limiting.
// Existing distortion may be propagated but never generates secondary products.
class CoherentHighOrderAmplifier {
    SaturatingFundamentalCompression fundamental_;
    SoftInputLimiter limiter_;
    std::vector<double> coefficients_;
    CoherentPolynomial polynomial_;
    double reference_;

    static std::vector<double> polynomial_coefficients(const std::vector<double> &nonlinear) {
        if (nonlinear.size() > maximum_polynomial_order - 1) {
            throw std::invalid_argument("high-order amplifier accepts at most a2 through a11");
        }
        std::vector<double> result{0., 0.};
        result.insert(result.end(), nonlinear.begin(), nonlinear.end());
        return result;
    }

public:
    CoherentHighOrderAmplifier(double power_gain_db,
                               double output_p1db_dbm,
                               double output_saturation_dbm,
                               const std::vector<double> &nonlinear_voltage_coefficients,
                               double reference_ohms = 50.)
        : fundamental_(power_gain_db, output_p1db_dbm, output_saturation_dbm),
          limiter_(std::pow(10., (output_p1db_dbm - power_gain_db - 34.) / 10.),
                   std::pow(10., (output_saturation_dbm - power_gain_db - 31.) / 10.)),
          coefficients_(polynomial_coefficients(nonlinear_voltage_coefficients)),
          polynomial_(coefficients_, reference_ohms), reference_(reference_ohms) {
    }

    FundamentalGainResponse fundamental_response(double power_w) const {
        return fundamental_.gain_response(power_w);
    }

    FundamentalGainResponse limiter_response(double power_w) const {
        return limiter_.gain_response(power_w);
    }

    // Full c0..cn vector; c0/c1 are zero because the direct gain is separate.
    const std::vector<double> &voltage_coefficients() const noexcept {
        return coefficients_;
    }

    double reference_ohms() const noexcept {
        return reference_;
    }

    AmplifierOperatingPoint operating_point(double total_input_power_w) const {
        const double gain = fundamental_.amplitude_gain(total_input_power_w);
        double scale = 1., limited_power = 0.;
        if (total_input_power_w > 0.) {
            const double drive = std::sqrt(total_input_power_w);
            const double limited = limiter_.limit({drive, 0.}).real();
            scale = limited / drive;
            limited_power = limited * limited;
        }
        return {gain,
                scale,
                limited_power,
                coefficients_.size() > 2 ? coefficients_[2] : 0.,
                coefficients_.size() > 3 ? coefficients_[3] : 0.};
    }

    CoherentHighOrderAmplifierResponse evaluate(double spacing_hz,
                                                const std::vector<CoherentComponent> &input,
                                                std::uint64_t reserved_group_max = 0,
                                                bool propagate_distortion = false) const {
        const auto reduced = reduce_coherent_components(spacing_hz, input);
        CoherentHighOrderAmplifierResponse result{
            reduced.components, {}, reduced.total_power_w, operating_point(reduced.total_power_w)};
        auto limited = result.inputs;
        for (std::size_t i = 0; i < result.inputs.size(); ++i) {
            const auto &component = result.inputs[i];
            if (component.kind != SpectrumKind::source && !propagate_distortion) {
                throw std::invalid_argument("high-order amplifier requires source-kind inputs");
            }
            limited[i].amplitude =
                component.kind == SpectrumKind::source
                    ? component.amplitude * result.operating_point.nonlinear_input_scale
                    : Complex{};
            auto direct = component;
            direct.amplitude *= result.operating_point.fundamental_amplitude_gain;
            CoherentPolynomialTerm term{};
            term.order = 1;
            term.input_indices[0] = static_cast<int>(i + 1);
            term.component = direct;
            result.terms.push_back(term);
        }
        // Zeroed distortion inputs remain in the reduced array, preserving the
        // original one-based indices and reserving every existing group ID.
        const auto generated = polynomial_.evaluate(spacing_hz, limited, reserved_group_max);
        if (result.terms.size() + generated.terms.size() > 4096) {
            throw std::length_error("high-order amplifier exceeds 4096 combined output terms");
        }
        result.terms.insert(result.terms.end(), generated.terms.begin(), generated.terms.end());
        std::sort(result.terms.begin(), result.terms.end(), [](const auto &a, const auto &b) {
            return std::tie(a.order, a.component.bin, a.input_indices) <
                   std::tie(b.order, b.component.bin, b.input_indices);
        });
        std::vector<CoherentComponent> output;
        for (const auto &term : result.terms) {
            output.push_back(term.component);
        }
        reduce_coherent_components(spacing_hz, output);
        return result;
    }
};
} // namespace rfmodel
