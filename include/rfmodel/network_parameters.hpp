#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
namespace parameter_detail {
// Solve A*X=B with partial pivoting; multiple right-hand sides.
inline SMatrix solve(SMatrix a, SMatrix b, double scale_floor = 0.) {
    const auto n = a.ports;
    const SMatrix original_a = a, original_b = b;
    double scale = 0;
    for (std::size_t r = 0; r < n; ++r) {
        double sum = 0;
        for (std::size_t c = 0; c < n; ++c) {
            sum += std::abs(a(r, c));
        }
        scale = std::max(scale, sum);
    }
    const double threshold =
        64 * std::numeric_limits<double>::epsilon() * n * std::max(scale, scale_floor);
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t pivot = k;
        for (std::size_t r = k + 1; r < n; ++r) {
            if (std::abs(a(r, k)) > std::abs(a(pivot, k))) {
                pivot = r;
            }
        }
        if (std::abs(a(pivot, k)) <= threshold) {
            throw std::domain_error("singular parameter conversion");
        }
        for (std::size_t c = 0; c < n; ++c) {
            std::swap(a(k, c), a(pivot, c));
            std::swap(b(k, c), b(pivot, c));
        }
        for (std::size_t r = k + 1; r < n; ++r) {
            const auto factor = a(r, k) / a(k, k);
            a(r, k) = 0;
            for (std::size_t c = k + 1; c < n; ++c) {
                a(r, c) -= factor * a(k, c);
            }
            for (std::size_t c = 0; c < n; ++c) {
                b(r, c) -= factor * b(k, c);
            }
        }
    }
    for (std::size_t r = n; r-- > 0;) {
        for (std::size_t col = 0; col < n; ++col) {
            for (std::size_t c = r + 1; c < n; ++c) {
                b(r, col) -= a(r, c) * b(c, col);
            }
            b(r, col) /= a(r, r);
        }
    }
    // Check every right-hand side against the original equations. Scale both
    // equations and solutions before products to avoid residual overflow.
    double equation_scale = 0., solution_scale = 1.;
    for (auto value : original_a.values) {
        equation_scale = std::max(equation_scale, std::abs(value));
    }
    for (auto value : original_b.values) {
        equation_scale = std::max(equation_scale, std::abs(value));
    }
    for (auto value : b.values) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
            throw std::overflow_error("parameter solution overflow");
        }
        solution_scale = std::max(solution_scale, std::abs(value));
    }
    if (!std::isfinite(equation_scale) || !std::isfinite(solution_scale)) {
        throw std::overflow_error("parameter residual scale overflow");
    }
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t col = 0; col < n; ++col) {
            const auto rhs = (original_b(r, col) / equation_scale) / solution_scale;
            Complex residual = -rhs;
            double denominator = std::abs(rhs);
            for (std::size_t c = 0; c < n; ++c) {
                const auto term =
                    (original_a(r, c) / equation_scale) * (b(c, col) / solution_scale);
                residual += term;
                denominator += std::abs(term);
            }
            if (std::abs(residual) >
                256 * std::numeric_limits<double>::epsilon() * n * denominator) {
                throw std::domain_error("parameter conversion residual exceeds tolerance");
            }
        }
    }
    return b;
}

inline SMatrix convert(const SMatrix &s, double reference, bool admittance) {
    if (!s.ports || s.ports > 1024 || s.values.size() != s.ports * s.ports ||
        !std::isfinite(reference) || reference <= 0) {
        throw std::invalid_argument("invalid conversion dimensions/reference");
    }
    for (auto x : s.values) {
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            throw std::invalid_argument("nonfinite S parameter");
        }
    }
    SMatrix a{s.ports, std::vector<Complex>(s.values.size())}, b = a;
    for (std::size_t r = 0; r < s.ports; ++r) {
        for (std::size_t c = 0; c < s.ports; ++c) {
            const Complex identity = r == c ? 1. : 0.;
            a(r, c) = identity + (admittance ? s(r, c) : -s(r, c));
            b(r, c) = identity + (admittance ? -s(r, c) : s(r, c));
        }
    }
    auto result = solve(a, b, 1.);
    for (auto &x : result.values) {
        x = admittance ? x / reference : x * reference;
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            throw std::overflow_error("parameter conversion overflow");
        }
    }
    return result;
}
} // namespace parameter_detail

// SMatrix is used as a dense complex matrix container; output units differ.
inline SMatrix s_to_z(const SMatrix &s, double reference_ohms = 50.) {
    return parameter_detail::convert(s, reference_ohms, false);
}

inline SMatrix s_to_y(const SMatrix &s, double reference_ohms = 50.) {
    return parameter_detail::convert(s, reference_ohms, true);
}

// Change a common positive real reference without converting through Z;
// the latter would be singular for ideal open circuits and thru networks.
inline SMatrix renormalize_s(const SMatrix &s, double old_reference, double new_reference) {
    if (!s.ports || s.ports > 1024 || s.values.size() != s.ports * s.ports ||
        !std::isfinite(old_reference) || old_reference <= 0 || !std::isfinite(new_reference) ||
        new_reference <= 0) {
        throw std::invalid_argument("invalid S renormalization input");
    }
    for (auto x : s.values) {
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            throw std::invalid_argument("nonfinite S parameter");
        }
    }
    if (old_reference == new_reference) {
        return s;
    }
    const double ratio =
        std::min(old_reference, new_reference) / std::max(old_reference, new_reference);
    const double magnitude = (1 - ratio) / (1 + ratio);
    if (magnitude == 1) {
        throw std::domain_error("reference ratio exceeds numerical resolution");
    }
    const double gamma = old_reference > new_reference ? magnitude : -magnitude;
    SMatrix a{s.ports, std::vector<Complex>(s.values.size())}, b = a;
    for (std::size_t r = 0; r < s.ports; ++r) {
        for (std::size_t c = 0; c < s.ports; ++c) {
            const double identity = r == c ? 1. : 0.;
            a(r, c) = identity + gamma * s(r, c);
            b(r, c) = s(r, c) + gamma * identity;
        }
    }
    // Preserve the identity scale when cancellation makes I+gamma*S tiny.
    auto result = parameter_detail::solve(a, b, 1.);
    for (auto x : result.values) {
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            throw std::overflow_error("renormalization overflow");
        }
    }
    return result;
}
} // namespace rfmodel
