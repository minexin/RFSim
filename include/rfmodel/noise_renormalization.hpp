#pragma once
#include "network_parameters.hpp"
#include "noise_matrix.hpp"

namespace rfmodel {
// Change the common real power-wave reference for b=S*a+c.
// S'=(S+gamma*I)*(I+gamma*S)^-1 and c'=T*c,
// T=sqrt(1-gamma^2)*(I+gamma*S)^-1; hence C'=T*C*T^H.
inline NoiseCorrelation renormalize_noise(const SMatrix &scattering,
                                          const NoiseCorrelation &intrinsic,
                                          double old_reference,
                                          double new_reference) {
    noise_detail::finite_matrix(scattering);
    if (scattering.ports != intrinsic.watts_per_hz.ports || !std::isfinite(old_reference) ||
        old_reference <= 0 || !std::isfinite(new_reference) || new_reference <= 0) {
        throw std::invalid_argument("invalid noise renormalization input");
    }
    const auto covariance = noise_detail::psd_matrix(intrinsic.watts_per_hz);
    if (old_reference == new_reference) {
        return {covariance};
    }
    const double ratio =
        std::min(old_reference, new_reference) / std::max(old_reference, new_reference);
    const double magnitude = (1 - ratio) / (1 + ratio);
    if (magnitude == 1) {
        throw std::domain_error("reference ratio exceeds numerical resolution");
    }
    const double gamma = old_reference > new_reference ? magnitude : -magnitude;
    const double normalization = 2 * std::sqrt(ratio) / (1 + ratio);
    const auto ports = scattering.ports;
    SMatrix feedback{ports, std::vector<Complex>(ports * ports)}, identity = feedback;
    for (std::size_t row = 0; row < ports; ++row) {
        identity(row, row) = normalization;
        for (std::size_t column = 0; column < ports; ++column) {
            feedback(row, column) = gamma * scattering(row, column);
        }
        feedback(row, row) += 1.;
    }
    const auto transfer = parameter_detail::solve(feedback, identity, 1.);
    return propagate_noise(transfer, {covariance});
}
} // namespace rfmodel
