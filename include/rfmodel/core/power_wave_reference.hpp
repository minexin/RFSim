#pragma once
#include "rfmodel/core/network_parameters.hpp"
#include "rfmodel/noise/noise_matrix.hpp"

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

struct Connection {
    Complex first_reflection, second_reflection, coupling;
};

// Vp=Vq and Ip=-Iq, with currents directed into each device. Inputs have
// already been validated. Scale before adding impedances or multiplying roots.
inline Connection connection(Complex first, Complex second) {
    const double scale =
        std::max({first.real(), second.real(), std::abs(first.imag()), std::abs(second.imag())});
    const auto p = first / scale, q = second / scale;
    const auto denominator = std::conj(p + q);
    const auto coupling = (2. * (std::sqrt(first.real()) / std::sqrt(scale)) *
                           (std::sqrt(second.real()) / std::sqrt(scale))) /
                          denominator;
    Connection result{(std::conj(q) - p) / denominator, (std::conj(p) - q) / denominator, coupling};
    for (const auto value : {result.first_reflection, result.second_reflection, result.coupling}) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
            throw std::runtime_error("power-wave connection coefficient overflow");
        }
    }
    return result;
}

struct Coordinates {
    Complex incident_from_incident, incident_from_outgoing;
    Complex outgoing_from_incident, outgoing_from_outgoing;
};

// New waves in terms of old waves at the same physical V/I. Both impedances
// have positive real parts. Splitting the imaginary ratio avoids root products.
inline Coordinates coordinates(Complex old_reference, Complex new_reference) {
    const double old_root = std::sqrt(old_reference.real());
    const double new_root = std::sqrt(new_reference.real());
    const double ratio = old_root / new_root, inverse = new_root / old_root;
    const double large = std::max(old_root, new_root), small = std::min(old_root, new_root);
    const double imaginary =
        .5 * (new_reference.imag() / large) / small - .5 * (old_reference.imag() / large) / small;
    const Complex a{.5 * ratio + .5 * inverse, imaginary};
    const Complex b{.5 * ratio - .5 * inverse, -imaginary};
    return {a, b, std::conj(b), std::conj(a)};
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
    std::vector<Complex> a_diagonal(s.ports), b(s.ports), d(s.ports);
    for (std::size_t row = 0; row < s.ports; ++row) {
        const auto change =
            power_wave_detail::coordinates(old_references[row], new_references[row]);
        const auto a = change.incident_from_incident, c = change.outgoing_from_incident;
        a_diagonal[row] = a;
        b[row] = change.incident_from_outgoing;
        d[row] = change.outgoing_from_outgoing;
        for (std::size_t column = 0; column < s.ports; ++column) {
            const double identity = row == column ? 1. : 0.;
            incident(row, column) = a * identity + b[row] * s(row, column);
            outgoing(row, column) = c * identity + d[row] * s(row, column);
        }
    }
    auto changed = power_wave_detail::right_solve(incident, outgoing, 1.);
    // Inverse coordinates give (A+S*B)*b_new=(C+S*D)*a_new+c_old.
    // Compute the noise transfer by a solve, not D-S_new*B: that subtraction
    // can erase a small finite transfer at very different reference impedances.
    SMatrix equation{s.ports, std::vector<Complex>(s.values.size())}, identity = equation;
    for (std::size_t row = 0; row < s.ports; ++row) {
        for (std::size_t column = 0; column < s.ports; ++column) {
            equation(row, column) =
                (row == column ? a_diagonal[row] : Complex{}) + s(row, column) * b[column];
        }
        identity(row, row) = 1.;
    }
    noise_detail::finite_matrix(equation);
    auto transfer = parameter_detail::solve(equation, identity, 1.);
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
