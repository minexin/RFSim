#pragma once
#include "rfmodel/core/device_model.hpp"
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
    // Per-RHS normwise backward error, with row-local equation scaling.
    // Structural zeros may retain tiny pivoting roundoff: normalizing by only
    // their nonzero residue incorrectly reports a relative error of one.
    double matrix_scale = 0.;
    for (auto value : original_a.values) {
        matrix_scale = std::max(matrix_scale, std::abs(value));
    }
    for (std::size_t col = 0; col < n; ++col) {
        double equation_scale = matrix_scale, column_scale = 0.;
        for (std::size_t r = 0; r < n; ++r) {
            if (!std::isfinite(b(r, col).real()) || !std::isfinite(b(r, col).imag())) {
                throw std::overflow_error("parameter solution overflow");
            }
            equation_scale = std::max(equation_scale, std::abs(original_b(r, col)));
            column_scale = std::max(column_scale, std::abs(b(r, col)));
        }
        if (!std::isfinite(equation_scale) || !std::isfinite(column_scale)) {
            throw std::overflow_error("parameter residual scale overflow");
        }
        const double solution_scale = std::max(1., column_scale);
        const double normalized_column = column_scale / solution_scale;
        for (std::size_t r = 0; r < n; ++r) {
            const auto rhs = (original_b(r, col) / equation_scale) / solution_scale;
            Complex residual = -rhs;
            double row_norm = 0.;
            for (std::size_t c = 0; c < n; ++c) {
                const auto coefficient = original_a(r, c) / equation_scale;
                residual += coefficient * (b(c, col) / solution_scale);
                row_norm += std::abs(coefficient);
            }
            const double denominator = std::abs(rhs) + row_norm * normalized_column;
            if (std::abs(residual) >
                256 * std::numeric_limits<double>::epsilon() * n * denominator) {
                throw std::domain_error("parameter conversion residual exceeds tolerance");
            }
        }
    }
    return b;
}

// Single right-hand side: no square RHS, inverse, or unused columns.
inline std::vector<Complex> solve(SMatrix a, std::vector<Complex> b, double scale_floor = 0.) {
    const auto n = a.ports;
    if (!n || n > 1024 || a.values.size() != n * n || b.size() != n ||
        !std::isfinite(scale_floor) || scale_floor < 0.) {
        throw std::invalid_argument("invalid vector parameter solve dimensions/scale");
    }
    for (const auto &values : {&a.values, &b}) {
        for (const auto value : *values) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                throw std::invalid_argument("nonfinite vector parameter solve input");
            }
        }
    }
    // Signal RHS underflow as well as overflow so callers can preserve a more
    // favorable operation order. A tiny backward error alone cannot detect it.
    const auto check_range = [](Complex value) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) ||
            std::fpclassify(value.real()) == FP_SUBNORMAL ||
            std::fpclassify(value.imag()) == FP_SUBNORMAL) {
            throw std::overflow_error("parameter RHS numeric range loss");
        }
    };
    const auto product = [&](Complex first, Complex second) {
        for (const double x : {first.real(), first.imag()}) {
            for (const double y : {second.real(), second.imag()}) {
                const double term = x * y;
                if (x != 0. && y != 0. && (term == 0. || std::fpclassify(term) == FP_SUBNORMAL)) {
                    throw std::overflow_error("parameter RHS product underflow");
                }
            }
        }
        const auto result = first * second;
        check_range(result);
        return result;
    };
    const SMatrix original_a = a;
    const auto original_b = b;
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
        }
        std::swap(b[k], b[pivot]);
        for (std::size_t r = k + 1; r < n; ++r) {
            const auto factor = a(r, k) / a(k, k);
            a(r, k) = 0;
            for (std::size_t c = k + 1; c < n; ++c) {
                a(r, c) -= factor * a(k, c);
            }
            b[r] -= product(factor, b[k]);
            check_range(b[r]);
        }
    }
    for (std::size_t r = n; r-- > 0;) {
        for (std::size_t c = r + 1; c < n; ++c) {
            b[r] -= product(a(r, c), b[c]);
            check_range(b[r]);
        }
        const auto before = b[r];
        b[r] /= a(r, r);
        check_range(b[r]);
        if (before != Complex{} && b[r] == Complex{}) {
            throw std::overflow_error("parameter RHS quotient underflow");
        }
    }
    // Per-RHS normwise backward error, with row-local equation scaling.
    // Structural zeros may retain tiny pivoting roundoff: normalizing by only
    // their nonzero residue incorrectly reports a relative error of one.
    double matrix_scale = 0.;
    for (auto value : original_a.values) {
        matrix_scale = std::max(matrix_scale, std::abs(value));
    }
    {
        double equation_scale = matrix_scale, column_scale = 0.;
        for (std::size_t r = 0; r < n; ++r) {
            if (!std::isfinite(b[r].real()) || !std::isfinite(b[r].imag())) {
                throw std::overflow_error("parameter solution overflow");
            }
            equation_scale = std::max(equation_scale, std::abs(original_b[r]));
            column_scale = std::max(column_scale, std::abs(b[r]));
        }
        if (!std::isfinite(equation_scale) || !std::isfinite(column_scale)) {
            throw std::overflow_error("parameter residual scale overflow");
        }
        const double solution_scale = std::max(1., column_scale);
        const double normalized_column = column_scale / solution_scale;
        for (std::size_t r = 0; r < n; ++r) {
            const auto rhs = (original_b[r] / equation_scale) / solution_scale;
            Complex residual = -rhs;
            double row_norm = 0.;
            for (std::size_t c = 0; c < n; ++c) {
                const auto coefficient = original_a(r, c) / equation_scale;
                residual += coefficient * (b[c] / solution_scale);
                row_norm += std::abs(coefficient);
            }
            const double denominator = std::abs(rhs) + row_norm * normalized_column;
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
