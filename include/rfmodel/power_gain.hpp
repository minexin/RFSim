#pragma once
#include "device_model.hpp"
#include <cmath>

namespace rfmodel {
namespace gain_detail {
inline void validate(const SMatrix &scattering) {
    if (scattering.ports != 2 || scattering.values.size() != 4) {
        throw std::invalid_argument("power gain requires a two-port S matrix");
    }
    for (const auto value : scattering.values) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
            throw std::invalid_argument("nonfinite S parameter");
        }
    }
}

inline double mismatch_factor(Complex reflection, bool allow_unit_magnitude) {
    const double magnitude = std::abs(reflection);
    if (!std::isfinite(magnitude) || magnitude > 1 || (!allow_unit_magnitude && magnitude == 1)) {
        throw std::invalid_argument("gain requires a passive termination with defined power");
    }
    return (1 - magnitude) * (1 + magnitude);
}

inline double squared_magnitude(Complex value) {
    const double power = std::norm(value);
    if (!std::isfinite(power)) {
        throw std::overflow_error("power gain intermediate overflow");
    }
    return power;
}

inline double ratio(double numerator, double denominator) {
    if (!std::isfinite(numerator) || !std::isfinite(denominator)) {
        throw std::overflow_error("power gain intermediate overflow");
    }
    if (denominator <= 0) {
        throw std::domain_error("power gain has no positive finite power reference");
    }
    const double gain = numerator / denominator;
    if (!std::isfinite(gain)) {
        throw std::overflow_error("power gain overflow");
    }
    return gain;
}
} // namespace gain_detail

// Linear ratio of load-delivered power to source-available power.
// Same positive real reference at both ports; |source|<1 and |load|<=1.
inline double transducer_power_gain(const SMatrix &s, Complex source = {}, Complex load = {}) {
    gain_detail::validate(s);
    const double source_factor = gain_detail::mismatch_factor(source, false);
    const double load_factor = gain_detail::mismatch_factor(load, true);
    const Complex determinant =
        (1. - s(0, 0) * source) * (1. - s(1, 1) * load) - s(0, 1) * s(1, 0) * source * load;
    return gain_detail::ratio(gain_detail::squared_magnitude(s(1, 0)) * source_factor * load_factor,
                              gain_detail::squared_magnitude(determinant));
}

// Linear ratio of load-delivered power to net power accepted at the input.
// Independent of source impedance, provided input accepted power is positive.
inline double operating_power_gain(const SMatrix &s, Complex load = {}) {
    gain_detail::validate(s);
    const double load_factor = gain_detail::mismatch_factor(load, true);
    const Complex load_denominator = 1. - s(1, 1) * load;
    const Complex input_numerator = s(0, 0) * load_denominator + s(0, 1) * s(1, 0) * load;
    const double accepted = gain_detail::squared_magnitude(load_denominator) -
                            gain_detail::squared_magnitude(input_numerator);
    return gain_detail::ratio(gain_detail::squared_magnitude(s(1, 0)) * load_factor, accepted);
}

// Linear ratio of output-available power to source-available power.
// Output conjugate matching must correspond to a passive load (|Gamma_out|<1).
inline double available_power_gain(const SMatrix &s, Complex source = {}) {
    gain_detail::validate(s);
    const double source_factor = gain_detail::mismatch_factor(source, false);
    const Complex source_denominator = 1. - s(0, 0) * source;
    const Complex output_numerator = s(1, 1) * source_denominator + s(0, 1) * s(1, 0) * source;
    const double available = gain_detail::squared_magnitude(source_denominator) -
                             gain_detail::squared_magnitude(output_numerator);
    return gain_detail::ratio(gain_detail::squared_magnitude(s(1, 0)) * source_factor, available);
}
} // namespace rfmodel
