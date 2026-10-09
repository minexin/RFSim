#pragma once
#include "noise_figure.hpp"
#include "power_wave_reference.hpp"

namespace rfmodel {
namespace power_wave_noise_detail {
inline double resistance_scale(Complex reference, Complex optimum) {
    if (!std::isfinite(optimum.real()) || !std::isfinite(optimum.imag()) ||
        std::abs(optimum) >= 1.) {
        throw std::invalid_argument("passive optimum source reflection required");
    }
    // Gamma is the source boundary a=Gamma*b, not the device ratio b/a.
    // Zsource=(Zref+conj(Zref)*Gamma)/(1-Gamma).
    const Complex ratio =
        (reference / reference.real() + (std::conj(reference) / reference.real()) * optimum) /
        (1. + optimum);
    const double scale = std::norm(ratio);
    if (!std::isfinite(scale) || scale <= 0.) {
        throw std::overflow_error("noise resistance reference scaling overflow/underflow");
    }
    return scale;
}

inline void two_port(const SMatrix &s, const std::vector<Complex> &references) {
    noise_detail::finite_matrix(s);
    if (s.ports != 2) {
        throw std::invalid_argument("power-wave noise analysis requires a two-port");
    }
    power_wave_detail::references(references, 2);
}
} // namespace power_wave_noise_detail

// Source impedance is physical ohms, with positive real part.
// Output termination is noiseless and has zero incident-wave reflection.
inline double power_wave_noise_figure_db(const SMatrix &s,
                                         const NoiseCorrelation &intrinsic,
                                         const std::vector<Complex> &references,
                                         Complex source_impedance_ohms,
                                         double temperature_k = 290.) {
    power_wave_noise_detail::two_port(s, references);
    power_wave_detail::references({source_impedance_ohms}, 1);
    // Scale before sums to avoid overflow for large finite impedances.
    const auto z = references[0];
    const double scale = std::max({std::abs(z.real()),
                                   std::abs(z.imag()),
                                   std::abs(source_impedance_ohms.real()),
                                   std::abs(source_impedance_ohms.imag())});
    const Complex source = source_impedance_ohms / scale, reference = z / scale;
    const Complex gamma = (source - reference) / (source + std::conj(reference));
    return two_port_noise_figure_db(s, intrinsic, gamma, temperature_k);
}

// NFmin and physical Rn are invariant under reference changes. GammaOpt is
// the source boundary coefficient a=GammaOpt*b in the specified power waves.
inline TwoPortNoiseParameters
extract_power_wave_noise_parameters(const SMatrix &s,
                                    const NoiseCorrelation &intrinsic,
                                    const std::vector<Complex> &references,
                                    double temperature_k = 290.) {
    power_wave_noise_detail::two_port(s, references);
    auto result = extract_noise_parameters(s, intrinsic, references[0].real(), temperature_k);
    if (result.noise_resistance_ohms != 0.) {
        result.noise_resistance_ohms *= power_wave_noise_detail::resistance_scale(
            references[0], result.optimum_source_reflection);
    }
    if (!std::isfinite(result.noise_resistance_ohms)) {
        throw std::overflow_error("physical noise resistance overflow");
    }
    return result;
}

inline NoiseCorrelation noise_from_power_wave_parameters(const SMatrix &s,
                                                         const TwoPortNoiseParameters &parameters,
                                                         const std::vector<Complex> &references,
                                                         double temperature_k = 290.) {
    power_wave_noise_detail::two_port(s, references);
    auto equivalent = parameters;
    const double scale = power_wave_noise_detail::resistance_scale(
        references[0], parameters.optimum_source_reflection);
    equivalent.noise_resistance_ohms /= scale;
    if (parameters.noise_resistance_ohms != 0. && equivalent.noise_resistance_ohms == 0.) {
        throw std::overflow_error("noise resistance underflow");
    }
    return noise_from_parameters(s, equivalent, references[0].real(), temperature_k);
}
} // namespace rfmodel
