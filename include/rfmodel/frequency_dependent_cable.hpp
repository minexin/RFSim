#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
// RFModel-defined, finite-frequency coaxial cable approximation. Each loss
// coefficient is its own dB/m contribution at reference_frequency_hz. Constant
// real characteristic impedance and nondispersive phase do not imply a causal
// broadband model. In particular, the DC limit does not model conductor resistance.
struct FrequencyDependentCableParameters {
    double length_m{1.};
    double characteristic_ohms{50.};
    double relative_permittivity{1.};
    double reference_frequency_hz{1e9};
    double sqrt_loss_db_per_m{};
    double linear_loss_db_per_m{};
    double reference_ohms{50.};
};

namespace cable_detail {
// A nonnegative binary-scaled value. Keeping the exponent separate avoids
// depending on a wider long double (which is not available on every C++17 target).
class ScaledPositive {
    double mantissa_{};
    int exponent_{};

    void normalize() {
        if (mantissa_ == 0.) {
            exponent_ = 0;
            return;
        }
        int adjustment{};
        mantissa_ = std::frexp(mantissa_, &adjustment);
        exponent_ += adjustment;
    }

public:
    explicit ScaledPositive(double value) : mantissa_(value) {
        normalize();
    }

    ScaledPositive &multiply(double value) {
        int exponent{};
        mantissa_ *= std::frexp(value, &exponent);
        exponent_ += exponent;
        normalize();
        return *this;
    }

    ScaledPositive &multiply(const ScaledPositive &value) {
        mantissa_ *= value.mantissa_;
        exponent_ += value.exponent_;
        normalize();
        return *this;
    }

    ScaledPositive &divide(double value) {
        int exponent{};
        mantissa_ /= std::frexp(value, &exponent);
        exponent_ -= exponent;
        normalize();
        return *this;
    }

    ScaledPositive &add(const ScaledPositive &other) {
        if (other.mantissa_ == 0.) {
            return *this;
        }
        if (mantissa_ == 0.) {
            *this = other;
            return *this;
        }
        const int exponent = std::max(exponent_, other.exponent_);
        mantissa_ = std::scalbn(mantissa_, exponent_ - exponent) +
                    std::scalbn(other.mantissa_, other.exponent_ - exponent);
        exponent_ = exponent;
        normalize();
        return *this;
    }

    double value() const {
        return std::scalbn(mantissa_, exponent_);
    }

    // exp(-alpha) is exactly rounded to zero well before alpha reaches 1024.
    // Avoid materializing arbitrarily large attenuation merely to exponentiate it.
    bool opaque() const {
        return mantissa_ != 0. && exponent_ > 11;
    }
};

inline double validate(const FrequencyDependentCableParameters &p) {
    for (const double value : {p.length_m, p.sqrt_loss_db_per_m, p.linear_loss_db_per_m}) {
        if (!std::isfinite(value) || value < 0.) {
            throw std::invalid_argument(
                "cable length and loss coefficients must be finite nonnegative");
        }
    }
    for (const double value : {p.characteristic_ohms,
                               p.relative_permittivity,
                               p.reference_frequency_hz,
                               p.reference_ohms}) {
        if (!std::isfinite(value) || value <= 0.) {
            throw std::invalid_argument(
                "cable impedances, permittivity and reference frequency must be finite positive");
        }
    }
    const double scale = std::max(p.characteristic_ohms, p.reference_ohms);
    const double line = p.characteristic_ohms / scale;
    const double reference = p.reference_ohms / scale;
    const double reflection = (line - reference) / (line + reference);
    if (1. - std::abs(reflection) <= 64 * std::numeric_limits<double>::epsilon()) {
        throw std::invalid_argument("cable impedance contrast exceeds numerical range");
    }
    return reflection;
}

inline void validate_frequency(double frequency_hz) {
    if (!std::isfinite(frequency_hz) || frequency_hz < 0.) {
        throw std::invalid_argument("cable frequency must be finite nonnegative");
    }
}

inline ScaledPositive propagation_loss(double frequency_hz,
                                       const FrequencyDependentCableParameters &p) {
    ScaledPositive conductor{p.length_m};
    conductor.multiply(p.sqrt_loss_db_per_m)
        .multiply(std::sqrt(frequency_hz))
        .divide(std::sqrt(p.reference_frequency_hz));
    ScaledPositive dielectric{p.length_m};
    dielectric.multiply(p.linear_loss_db_per_m)
        .multiply(frequency_hz)
        .divide(p.reference_frequency_hz);
    return conductor.add(dielectric);
}
} // namespace cable_detail

// One-way propagation loss, not the insertion loss of a mismatched two-port.
// Throws overflow_error if the final dB value exceeds the finite double range.
inline double cable_propagation_loss_db(double frequency_hz,
                                        const FrequencyDependentCableParameters &parameters) {
    cable_detail::validate(parameters);
    cable_detail::validate_frequency(frequency_hz);
    const double loss = cable_detail::propagation_loss(frequency_hz, parameters).value();
    if (!std::isfinite(loss)) {
        throw std::overflow_error("cable propagation loss outside numerical range");
    }
    return loss;
}

// Full reciprocal two-port S at a common, positive real reference impedance.
// The mathematical zero-length/DC limit is an ideal through connection. Finite
// input parameters may yield zero transmission by attenuation underflow; phase
// cycles must still be representable, even in that opaque limit.
inline SMatrix
frequency_dependent_cable_s_parameters(double frequency_hz,
                                       const FrequencyDependentCableParameters &parameters) {
    const double reflection = cable_detail::validate(parameters);
    cable_detail::validate_frequency(frequency_hz);
    if (parameters.length_m == 0. || frequency_hz == 0.) {
        return {2, {0., 1., 1., 0.}};
    }
    constexpr double light_speed_m_per_s = 299792458.;
    constexpr double two_pi = 6.28318530717958647692;
    constexpr double db_to_nepers = 0.11512925464970228420;
    cable_detail::ScaledPositive cycles{parameters.length_m};
    cycles.multiply(std::sqrt(parameters.relative_permittivity))
        .multiply(frequency_hz)
        .divide(light_speed_m_per_s);
    const double cycle_value = cycles.value();
    if (!std::isfinite(cycle_value)) {
        throw std::overflow_error("cable delay phase outside numerical range");
    }
    auto scaled_phase = cycles;
    scaled_phase.multiply(two_pi);
    const bool small_phase = cycle_value < 0.125;
    const double reduced_cycles = std::remainder(cycle_value, 1.);
    const int quadrant = static_cast<int>(std::round(4. * reduced_cycles));
    // Reduce in turns before multiplying by pi. Exact quarter/half turns must
    // not acquire sin(pi) residuals, which a high impedance contrast amplifies.
    const double phase =
        small_phase ? -scaled_phase.value() : -two_pi * (reduced_cycles - 0.25 * quadrant);
    double sine = std::sin(phase);
    double cosine = std::cos(phase);
    if (!small_phase) {
        const double original_sine = sine;
        if (quadrant == 1) {
            sine = -cosine;
            cosine = original_sine;
        } else if (quadrant == -1) {
            sine = cosine;
            cosine = -original_sine;
        } else if (quadrant == 2 || quadrant == -2) {
            sine = -sine;
            cosine = -cosine;
        }
    }
    auto attenuation = cable_detail::propagation_loss(frequency_hz, parameters);
    attenuation.multiply(db_to_nepers);
    // This is the physical opaque-line limit, including its input mismatch.
    if (attenuation.opaque()) {
        return {2, {reflection, 0., 0., reflection}};
    }
    const double alpha = attenuation.value();
    const double magnitude = std::exp(-alpha);
    if (magnitude == 0.) {
        return {2, {reflection, 0., 0., reflection}};
    }
    const Complex propagation = magnitude * Complex{cosine, sine};
    const double round_trip_magnitude = std::exp(-2. * alpha);
    const double impedance_ratio =
        std::min(parameters.characteristic_ohms, parameters.reference_ohms) /
        std::max(parameters.characteristic_ohms, parameters.reference_ohms);
    const double impedance_sum = 1. + impedance_ratio;
    // Derive coupling directly from impedances, not a nearly unit reflection:
    // (1-r)*(1+r) would amplify rounding near the accepted contrast limit.
    const double coupling = 4. * impedance_ratio / (impedance_sum * impedance_sum);
    auto normalized_attenuation = attenuation;
    normalized_attenuation.multiply(2.).divide(coupling);
    const double attenuation_defect = normalized_attenuation.value() *
                                      (alpha == 0. ? 1. : -std::expm1(-2. * alpha) / (2. * alpha));
    Complex normalized_defect;
    if (small_phase) {
        // Normalize before rounding extremely short phase/loss to double. This
        // preserves representable mismatch reflections even at subnormal scales.
        auto phase_square = scaled_phase;
        phase_square.multiply(scaled_phase).multiply(2.).divide(coupling);
        auto twice_phase = scaled_phase;
        twice_phase.multiply(2.).divide(coupling);
        const double sinc = phase == 0. ? 1. : std::sin(phase) / phase;
        const double sinc_twice = phase == 0. ? 1. : std::sin(2. * phase) / (2. * phase);
        normalized_defect = {attenuation_defect +
                                 round_trip_magnitude * phase_square.value() * sinc * sinc,
                             round_trip_magnitude * twice_phase.value() * sinc_twice};
    } else {
        normalized_defect = {attenuation_defect +
                                 2. * round_trip_magnitude * (sine / coupling) * sine,
                             -2. * round_trip_magnitude * (cosine / coupling) * sine};
    }
    // Same uniform-line scattering physics as TransmissionLineModel. Normalizing
    // first also avoids losing subnormal propagation in coupling*propagation.
    const Complex denominator = 1. + reflection * reflection * normalized_defect;
    const Complex reflected = reflection * normalized_defect / denominator;
    const Complex transmitted = propagation / denominator;
    return {2, {reflected, transmitted, transmitted, reflected}};
}

class FrequencyDependentCableModel final : public RFDeviceModel, public SParameterProvider {
    std::string name_;
    FrequencyDependentCableParameters parameters_;

public:
    FrequencyDependentCableModel(std::string name, FrequencyDependentCableParameters parameters)
        : name_(std::move(name)), parameters_(parameters) {
        if (name_.empty()) {
            throw std::invalid_argument("cable name must not be empty");
        }
        cable_detail::validate(parameters_);
    }

    std::string name() const override {
        return name_;
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        if (index >= 2) {
            throw std::out_of_range("cable port");
        }
        return {index, index == 0 ? "in" : "out", {parameters_.reference_ohms, 0.}};
    }

    SMatrix s_parameters(double frequency_hz) const override {
        return frequency_dependent_cable_s_parameters(frequency_hz, parameters_);
    }
};
} // namespace rfmodel
