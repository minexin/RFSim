#pragma once
#include "network_parameters.hpp"
#include "noise_matrix.hpp"

namespace rfmodel {
struct LoadedNoiseResult {
    NoiseCorrelation incident;
    NoiseCorrelation outgoing;
    // Positive means net flow into the device; negative means into its termination.
    std::vector<double> net_into_device_w_per_hz;
};

// b=S*a+c, a=Gamma*b+e. Intrinsic c and boundary emission e are independent;
// each may have arbitrary within-vector covariance. Gamma is diagonal.
inline LoadedNoiseResult loaded_noise(const SMatrix &scattering,
                                      const NoiseCorrelation &intrinsic,
                                      const std::vector<Complex> &reflections,
                                      const NoiseCorrelation &boundary_emission) {
    noise_detail::finite_matrix(scattering);
    const auto n = scattering.ports;
    if (reflections.size() != n || intrinsic.watts_per_hz.ports != n ||
        boundary_emission.watts_per_hz.ports != n) {
        throw std::invalid_argument("loaded noise dimensions differ");
    }
    const NoiseCorrelation c{noise_detail::psd_matrix(intrinsic.watts_per_hz)};
    const NoiseCorrelation e{noise_detail::psd_matrix(boundary_emission.watts_per_hz)};
    SMatrix feedback{n, std::vector<Complex>(n * n)}, identity = feedback;
    for (std::size_t row = 0; row < n; ++row) {
        const auto gamma = reflections[row];
        if (!std::isfinite(gamma.real()) || !std::isfinite(gamma.imag()) || std::abs(gamma) > 1.) {
            throw std::invalid_argument("loaded noise requires passive boundary reflections");
        }
        identity(row, row) = 1.;
        for (std::size_t column = 0; column < n; ++column) {
            feedback(row, column) =
                (row == column ? 1. : 0.) - scattering(row, column) * reflections[column];
        }
    }
    // Outgoing transfers H=(I-S*Gamma)^-1 and J=H*S.
    const auto h = parameter_detail::solve(feedback, identity, 1.);
    const auto j = parameter_detail::solve(feedback, scattering, 1.);
    SMatrix incident_c = h, incident_e = j;
    for (std::size_t row = 0; row < n; ++row) {
        for (std::size_t column = 0; column < n; ++column) {
            incident_c(row, column) *= reflections[row];
            incident_e(row, column) = reflections[row] * j(row, column) + (row == column ? 1. : 0.);
        }
    }
    auto out = propagate_noise(h, c).watts_per_hz;
    const auto out_e = propagate_noise(j, e).watts_per_hz;
    auto in = propagate_noise(incident_c, c).watts_per_hz;
    const auto in_e = propagate_noise(incident_e, e).watts_per_hz;
    for (std::size_t index = 0; index < n * n; ++index) {
        out.values[index] += out_e.values[index];
        in.values[index] += in_e.values[index];
    }
    LoadedNoiseResult result{{noise_detail::psd_matrix(in)}, {noise_detail::psd_matrix(out)}, {}};
    for (std::size_t port = 0; port < n; ++port) {
        result.net_into_device_w_per_hz.push_back(result.incident.watts_per_hz(port, port).real() -
                                                  result.outgoing.watts_per_hz(port, port).real());
    }
    return result;
}

// Independent passive one-port terminations emit k*T*(1-|Gamma|^2).
inline NoiseCorrelation thermal_boundary_noise(const std::vector<Complex> &reflections,
                                               const std::vector<double> &temperatures_k) {
    const auto n = reflections.size();
    if (n == 0 || n > 1024 || temperatures_k.size() != n) {
        throw std::invalid_argument("invalid thermal boundary dimensions");
    }
    NoiseCorrelation result{{n, std::vector<Complex>(n * n)}};
    constexpr double boltzmann = 1.380649e-23;
    for (std::size_t port = 0; port < n; ++port) {
        const auto gamma = reflections[port];
        const double temperature = temperatures_k[port];
        if (!std::isfinite(gamma.real()) || !std::isfinite(gamma.imag()) || std::abs(gamma) > 1. ||
            !std::isfinite(temperature) || temperature < 0.) {
            throw std::invalid_argument("invalid thermal boundary reflection or temperature");
        }
        result.watts_per_hz(port, port) =
            boltzmann * temperature * std::max(0., 1. - std::norm(gamma));
    }
    return result;
}
} // namespace rfmodel
