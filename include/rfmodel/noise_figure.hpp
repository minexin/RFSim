#pragma once
#include "noise_matrix.hpp"

namespace rfmodel {
// Forward (port 0 -> 1), noiseless matched output, passive source |Gamma|<1.
// Intrinsic covariance excludes the source and output termination noise.
inline double two_port_noise_figure_db(const SMatrix &s,
                                       const NoiseCorrelation &intrinsic,
                                       Complex source_reflection = {},
                                       double reference_temperature_k = 290.) {
    noise_detail::finite_matrix(s);
    if (s.ports != 2 || intrinsic.watts_per_hz.ports != 2) {
        throw std::invalid_argument("noise figure requires a two-port");
    }
    if (!std::isfinite(source_reflection.real()) || !std::isfinite(source_reflection.imag()) ||
        std::abs(source_reflection) >= 1. || !std::isfinite(reference_temperature_k) ||
        reference_temperature_k <= 0) {
        throw std::invalid_argument("invalid noise figure source/reference");
    }
    if (s(1, 0) == Complex{}) {
        throw std::domain_error("zero forward transmission");
    }
    const auto feedback = s(0, 0) * source_reflection;
    const auto denominator = 1. - feedback;
    if (!std::isfinite(feedback.real()) || !std::isfinite(feedback.imag())) {
        throw std::overflow_error("noise figure feedback overflow");
    }
    if (std::abs(denominator) <=
        64 * std::numeric_limits<double>::epsilon() * std::max(1., std::abs(feedback))) {
        throw std::domain_error("singular noise figure source feedback");
    }
    auto normalized = intrinsic;
    noise_detail::finite_matrix(normalized.watts_per_hz);
    constexpr double boltzmann = 1.380649e-23;
    const double kt = boltzmann * reference_temperature_k;
    if (kt == 0) {
        throw std::overflow_error("noise reference underflow");
    }
    for (auto &value : normalized.watts_per_hz.values) {
        value /= kt;
    }
    // Refer c2 back to the source plane; c1 is reflected by the source.
    const SMatrix weights{2, {source_reflection, denominator / s(1, 0), 0., 0.}};
    const double added = propagate_noise(weights, normalized).watts_per_hz(0, 0).real();
    const double excess = added / (1. - std::norm(source_reflection));
    if (!std::isfinite(excess)) {
        throw std::overflow_error("noise factor overflow");
    }
    return 10. / std::log(10.) * std::log1p(excess);
}

struct TwoPortNoiseParameters {
    double minimum_noise_figure_db{};
    Complex optimum_source_reflection{};
    double noise_resistance_ohms{};
};

inline NoiseCorrelation noise_from_parameters(const SMatrix &s,
                                              const TwoPortNoiseParameters &parameters,
                                              double reference_ohms = 50.,
                                              double temperature_k = 290.) {
    noise_detail::finite_matrix(s);
    const auto gamma = parameters.optimum_source_reflection;
    if (s.ports != 2 || !std::isfinite(reference_ohms) || reference_ohms <= 0 ||
        !std::isfinite(temperature_k) || temperature_k <= 0 ||
        !std::isfinite(parameters.minimum_noise_figure_db) ||
        parameters.minimum_noise_figure_db < 0 ||
        !std::isfinite(parameters.noise_resistance_ohms) || parameters.noise_resistance_ohms < 0 ||
        !std::isfinite(gamma.real()) || !std::isfinite(gamma.imag()) || std::abs(gamma) >= 1.) {
        throw std::invalid_argument("invalid two-port noise parameters");
    }
    if (s(1, 0) == Complex{}) {
        throw std::domain_error("zero forward transmission");
    }
    const double excess = std::expm1(parameters.minimum_noise_figure_db * std::log(10.) / 10.);
    const double curvature =
        4. * (parameters.noise_resistance_ohms / reference_ohms) / std::norm(1. + gamma);
    if (!std::isfinite(excess) || !std::isfinite(curvature)) {
        throw std::overflow_error("noise parameter conversion overflow");
    }
    // F-1 = excess + curvature*|Gamma-gamma|^2/(1-|Gamma|^2).
    // The resulting quadratic form must be PSD; positive scalar parameters
    // alone do not guarantee a physically admissible correlation matrix.
    const NoiseCorrelation referred{SMatrix{2,
                                            {curvature - excess,
                                             -curvature * std::conj(gamma),
                                             -curvature * gamma,
                                             excess + curvature * std::norm(gamma)}}};
    const SMatrix inverse{2, {1., s(0, 0), 0., s(1, 0)}};
    auto result = propagate_noise(inverse, referred);
    const double kt = 1.380649e-23 * temperature_k;
    if (kt == 0) {
        throw std::overflow_error("noise reference underflow");
    }
    for (auto &value : result.watts_per_hz.values) {
        value *= kt;
    }
    noise_detail::finite_matrix(result.watts_per_hz);
    return result;
}

inline TwoPortNoiseParameters extract_noise_parameters(const SMatrix &s,
                                                       const NoiseCorrelation &intrinsic,
                                                       double reference_ohms = 50.,
                                                       double temperature_k = 290.) {
    if (!std::isfinite(reference_ohms) || reference_ohms <= 0) {
        throw std::invalid_argument("invalid noise parameter reference");
    }
    // Validate dimensions, temperature, transmission and covariance first.
    two_port_noise_figure_db(s, intrinsic, {}, temperature_k);
    auto normalized = intrinsic;
    for (auto &x : normalized.watts_per_hz.values) {
        x /= (1.380649e-23 * temperature_k);
    }
    // Input-referred numerator = A*|Gamma|^2 + 2*Re(B*Gamma) + D.
    const SMatrix transform{2, {1., -s(0, 0) / s(1, 0), 0., 1. / s(1, 0)}};
    const auto q = propagate_noise(transform, normalized).watts_per_hz;
    const double scale = std::max(q(0, 0).real(), q(1, 1).real());
    if (scale == 0) {
        return {}; // Any source is optimal; choose matched source.
    }
    const double a = q(0, 0).real() / scale, d = q(1, 1).real() / scale;
    const Complex b = q(0, 1) / scale;
    const double sum = a + d, magnitude = std::abs(b);
    const double discriminant = std::max(0., (sum - 2 * magnitude) * (sum + 2 * magnitude));
    const Complex optimum = -2. * std::conj(b) / (sum + std::sqrt(discriminant));
    if (std::abs(optimum) >= 1. - 64 * std::numeric_limits<double>::epsilon()) {
        throw std::domain_error("noise minimum lies at unresolved unit-circle boundary");
    }
    const double minimum = two_port_noise_figure_db(s, intrinsic, optimum, temperature_k);
    const double resistance = (reference_ohms / 4.) * scale * (sum - 2 * b.real());
    if (!std::isfinite(resistance)) {
        throw std::overflow_error("noise resistance overflow");
    }
    return {minimum, optimum, resistance};
}
} // namespace rfmodel
