#pragma once
#include "network_parameters.hpp"
#include "noise_matrix.hpp"

namespace rfmodel {
namespace power_wave_detail {
inline void references(const std::vector<Complex> &values, std::size_t ports) {
    if (values.size() != ports) {
        throw std::invalid_argument("one reference impedance is required per port");
    }
    for (auto value : values) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) || value.real() <= 0.) {
            throw std::invalid_argument("power-wave references require finite positive real parts");
        }
    }
}

inline SMatrix transpose(const SMatrix &m) {
    SMatrix result{m.ports, std::vector<Complex>(m.values.size())};
    for (std::size_t row = 0; row < m.ports; ++row) {
        for (std::size_t column = 0; column < m.ports; ++column) {
            result(row, column) = m(column, row);
        }
    }
    return result;
}

// X*A=B. Plain transpose, not Hermitian transpose.
inline SMatrix right_solve(const SMatrix &a, const SMatrix &b, double scale_floor = 0.) {
    noise_detail::finite_matrix(a);
    noise_detail::finite_matrix(b);
    return transpose(parameter_detail::solve(transpose(a), transpose(b), scale_floor));
}

inline SMatrix parameters(const SMatrix &s, const std::vector<Complex> &z, bool admittance) {
    noise_detail::finite_matrix(s);
    references(z, s.ports);
    SMatrix voltage{s.ports, std::vector<Complex>(s.values.size())}, current = voltage;
    for (std::size_t row = 0; row < s.ports; ++row) {
        const double root = std::sqrt(z[row].real());
        for (std::size_t column = 0; column < s.ports; ++column) {
            const double identity = row == column ? 1. : 0.;
            voltage(row, column) =
                (std::conj(z[row]) / root) * identity + (z[row] / root) * s(row, column);
            current(row, column) = (identity - s(row, column)) / root;
        }
    }
    return admittance ? right_solve(voltage, current) : right_solve(current, voltage);
}
} // namespace power_wave_detail

struct PowerWaveRenormalization {
    SMatrix scattering;
    SMatrix noise_transfer;
};

// Kurokawa power waves: a=(V+Zref*I)/(2*sqrt(Re Zref)),
// b=(V-conj(Zref)*I)/(2*sqrt(Re Zref)). V/I use RMS phasors.
// Direct change of wave coordinates also supports ideal open/short/thru S.
inline PowerWaveRenormalization
renormalize_power_waves(const SMatrix &s,
                        const std::vector<Complex> &old_references,
                        const std::vector<Complex> &new_references) {
    noise_detail::finite_matrix(s);
    power_wave_detail::references(old_references, s.ports);
    power_wave_detail::references(new_references, s.ports);
    SMatrix incident{s.ports, std::vector<Complex>(s.values.size())}, outgoing = incident;
    std::vector<Complex> b(s.ports), d(s.ports);
    for (std::size_t row = 0; row < s.ports; ++row) {
        const double old_root = std::sqrt(old_references[row].real());
        const double new_root = std::sqrt(new_references[row].real());
        const double ratio = old_root / new_root;
        const double inverse = new_root / old_root;
        const double large = std::max(old_root, new_root);
        const double small = std::min(old_root, new_root);
        const double imaginary = .5 * (new_references[row].imag() / large) / small -
                                 .5 * (old_references[row].imag() / large) / small;
        const Complex a{.5 * ratio + .5 * inverse, imaginary};
        b[row] = {.5 * ratio - .5 * inverse, -imaginary};
        const Complex c = std::conj(b[row]);
        d[row] = std::conj(a);
        for (std::size_t column = 0; column < s.ports; ++column) {
            const double identity = row == column ? 1. : 0.;
            incident(row, column) = a * identity + b[row] * s(row, column);
            outgoing(row, column) = c * identity + d[row] * s(row, column);
        }
    }
    auto changed = power_wave_detail::right_solve(incident, outgoing, 1.);
    SMatrix transfer{s.ports, std::vector<Complex>(s.values.size())};
    for (std::size_t row = 0; row < s.ports; ++row) {
        for (std::size_t column = 0; column < s.ports; ++column) {
            transfer(row, column) =
                (row == column ? d[row] : Complex{}) - changed(row, column) * b[column];
        }
    }
    noise_detail::finite_matrix(transfer);
    return {std::move(changed), std::move(transfer)};
}

inline NoiseCorrelation renormalize_noise(const SMatrix &s,
                                          const NoiseCorrelation &intrinsic,
                                          const std::vector<Complex> &old_references,
                                          const std::vector<Complex> &new_references) {
    const auto transform = renormalize_power_waves(s, old_references, new_references);
    return propagate_noise(transform.noise_transfer, intrinsic);
}

inline SMatrix s_to_z(const SMatrix &s, const std::vector<Complex> &references) {
    return power_wave_detail::parameters(s, references, false);
}

inline SMatrix s_to_y(const SMatrix &s, const std::vector<Complex> &references) {
    return power_wave_detail::parameters(s, references, true);
}
} // namespace rfmodel
