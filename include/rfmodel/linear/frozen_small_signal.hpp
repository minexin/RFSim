#pragma once
#include "rfmodel/nonlinear/dc_operating_point.hpp"
#include "rfmodel/linear/network.hpp"

namespace rfmodel {
namespace frozen_detail {
using Wide = std::complex<long double>;

inline Wide checked(Wide value) {
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
        throw std::overflow_error("frozen small-signal numeric range exceeded");
    }
    return value;
}

// Reject intermediate range loss explicitly. Wider long double is opportunistic:
// platforms where it equals double may reject more extreme inputs. No promise
// is made that every representable final answer has representable intermediates.
inline long double real_product(long double a, long double b) {
    const auto result = a * b;
    if (!std::isfinite(result) ||
        (a != 0 && b != 0 && (result == 0 || std::fpclassify(result) == FP_SUBNORMAL))) {
        throw std::overflow_error("small-signal product numeric range loss");
    }
    return result;
}

inline long double real_divide(long double a, long double b) {
    const auto result = a / b;
    if (!std::isfinite(result) ||
        (a != 0 && (result == 0 || std::fpclassify(result) == FP_SUBNORMAL))) {
        throw std::overflow_error("small-signal quotient numeric range loss");
    }
    return result;
}

inline Wide product(Wide a, Wide b) {
    return checked({real_product(a.real(), b.real()) - real_product(a.imag(), b.imag()),
                    real_product(a.real(), b.imag()) + real_product(a.imag(), b.real())});
}

// Smith division, with checked scalar operations on both output components.
inline Wide divide(Wide a, Wide b) {
    if (std::abs(b.real()) >= std::abs(b.imag())) {
        const auto ratio = real_divide(b.imag(), b.real());
        const auto denominator = checked({b.real() + real_product(b.imag(), ratio), 0}).real();
        const auto numerator = checked(
            {a.real() + real_product(a.imag(), ratio), a.imag() - real_product(a.real(), ratio)});
        return {real_divide(numerator.real(), denominator),
                real_divide(numerator.imag(), denominator)};
    }
    const auto ratio = real_divide(b.real(), b.imag());
    const auto denominator = checked({b.imag() + real_product(b.real(), ratio), 0}).real();
    const auto numerator = checked(
        {real_product(a.real(), ratio) + a.imag(), real_product(a.imag(), ratio) - a.real()});
    return {real_divide(numerator.real(), denominator), real_divide(numerator.imag(), denominator)};
}

inline Complex narrow(Wide value) {
    checked(value);
    const long double limit = std::numeric_limits<double>::max();
    if (std::abs(value.real()) > limit || std::abs(value.imag()) > limit) {
        throw std::overflow_error("frozen small-signal result overflow");
    }
    const double re = static_cast<double>(value.real());
    const double im = static_cast<double>(value.imag());
    if (!std::isfinite(re) || !std::isfinite(im) || (value.real() != 0 && re == 0) ||
        (value.imag() != 0 && im == 0) || std::fpclassify(re) == FP_SUBNORMAL ||
        std::fpclassify(im) == FP_SUBNORMAL) {
        throw std::overflow_error("frozen small-signal result numeric range loss");
    }
    return {re, im};
}

inline void validate(const SMatrix &y) {
    if (!y.ports || y.ports > 1024 || y.values.size() != y.ports * y.ports) {
        throw std::invalid_argument("admittance dimensions must be square, 1..1024 ports");
    }
    for (auto v : y.values) {
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) {
            throw std::invalid_argument("nonfinite admittance");
        }
    }
}

// Left solve with rectangular RHS, no inverse. Intermediate range failures
// are explicit; support for extreme inputs may differ with long-double range.
inline std::vector<Wide> solve(std::vector<Wide> a,
                               std::vector<Wide> b,
                               std::size_t n,
                               std::size_t columns,
                               long double scale_floor = 0,
                               bool double_result = false) {
    const auto original_a = a, original_b = b;
    long double norm = 0;
    for (std::size_t r = 0; r < n; ++r) {
        long double sum = 0;
        for (std::size_t c = 0; c < n; ++c) {
            sum += std::abs(checked(a[r * n + c]));
        }
        if (!std::isfinite(sum)) {
            throw std::overflow_error("small-signal matrix norm overflow");
        }
        norm = std::max(norm, sum);
    }
    const auto threshold = real_product(64.L * std::numeric_limits<double>::epsilon() * n,
                                        std::max(norm, scale_floor));
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t p = k;
        for (std::size_t r = k + 1; r < n; ++r) {
            if (std::abs(a[r * n + k]) > std::abs(a[p * n + k])) {
                p = r;
            }
        }
        if (std::abs(a[p * n + k]) <= threshold) {
            throw std::domain_error("singular or numerically ill-conditioned small-signal system");
        }
        for (std::size_t c = 0; c < n; ++c) {
            std::swap(a[k * n + c], a[p * n + c]);
        }
        for (std::size_t c = 0; c < columns; ++c) {
            std::swap(b[k * columns + c], b[p * columns + c]);
        }
        for (std::size_t r = k + 1; r < n; ++r) {
            const auto factor = divide(a[r * n + k], a[k * n + k]);
            a[r * n + k] = {};
            for (std::size_t c = k + 1; c < n; ++c) {
                a[r * n + c] = checked(a[r * n + c] - product(factor, a[k * n + c]));
            }
            for (std::size_t c = 0; c < columns; ++c) {
                b[r * columns + c] =
                    checked(b[r * columns + c] - product(factor, b[k * columns + c]));
            }
        }
    }
    for (std::size_t r = n; r-- > 0;) {
        for (std::size_t c = 0; c < columns; ++c) {
            for (std::size_t k = r + 1; k < n; ++k) {
                b[r * columns + c] =
                    checked(b[r * columns + c] - product(a[r * n + k], b[k * columns + c]));
            }
            b[r * columns + c] = divide(b[r * columns + c], a[r * n + r]);
        }
    }
    // Check the actual rounded public S result against the original equations.
    // Schur X remains wide because only its final reduced Y is public.
    if (double_result) {
        for (auto &value : b) {
            value = Wide(narrow(value));
        }
    }
    for (std::size_t c = 0; c < columns; ++c) {
        long double xnorm = 0, bnorm = 0, error = 0;
        for (std::size_t r = 0; r < n; ++r) {
            xnorm = std::max(xnorm, std::abs(b[r * columns + c]));
            bnorm = std::max(bnorm, std::abs(original_b[r * columns + c]));
            auto residual = -original_b[r * columns + c];
            for (std::size_t k = 0; k < n; ++k) {
                residual = checked(residual + product(original_a[r * n + k], b[k * columns + c]));
            }
            error = std::max(error, std::abs(residual));
        }
        const auto denominator = real_product(norm, xnorm) + bnorm;
        if (!std::isfinite(denominator)) {
            throw std::overflow_error("small-signal residual scale overflow");
        }
        if (error > real_product(256.L * std::numeric_limits<double>::epsilon() * n, denominator)) {
            throw std::domain_error("small-signal solve residual exceeds tolerance");
        }
    }
    return b;
}
} // namespace frozen_detail

// RMS Kurokawa power waves; current flows into the network. Direct left solve
// (I + D Y D^-1 Z) S = I - D Y D^-1 conj(Z). Y may be singular, even zero.
inline SMatrix admittance_to_power_wave_s(const SMatrix &y,
                                          const std::vector<Complex> &references_ohm) {
    frozen_detail::validate(y);
    power_wave_detail::references(references_ohm, y.ports);
    using frozen_detail::Wide;
    const auto n = y.ports;
    std::vector<Wide> a(n * n), b(n * n);
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            const long double ratio = frozen_detail::real_divide(
                std::sqrt(static_cast<long double>(references_ohm[r].real())),
                std::sqrt(static_cast<long double>(references_ohm[c].real())));
            const auto coefficient = frozen_detail::product(Wide(y(r, c)), Wide(ratio));
            const Wide z = references_ohm[c];
            const Wide identity = r == c ? 1.L : 0.L;
            a[r * n + c] =
                frozen_detail::checked(identity + frozen_detail::product(coefficient, z));
            b[r * n + c] = frozen_detail::checked(
                identity - frozen_detail::product(coefficient, std::conj(z)));
        }
    }
    const auto solution = frozen_detail::solve(std::move(a), std::move(b), n, n, 1.L, true);
    SMatrix s{n, std::vector<Complex>(n * n)};
    for (std::size_t k = 0; k < n * n; ++k) {
        s.values[k] = frozen_detail::narrow(solution[k]);
    }
    return s;
}

class FrozenSmallSignal {
    DcOperatingPoint point_; // Numeric owned snapshot, never a device callback.

    explicit FrozenSmallSignal(const DcOperatingPoint &point) : point_(point) {
    }

    friend FrozenSmallSignal freeze_small_signal(const DcOperatingPoint &);

public:
    const DcOperatingPoint &operating_point() const noexcept {
        return point_;
    }

    // Rows are current equations, columns are grounded nodal voltages (node n -> n-1).
    SMatrix nodal_y(double frequency_hz) const {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0) {
            throw std::invalid_argument("small-signal frequency must be finite and nonnegative");
        }
        const long double omega = frozen_detail::real_product(2.L * std::acos(-1.L), frequency_hz);
        // Frequency itself must have a representable angular frequency, even if C=0.
        (void)frozen_detail::narrow({omega, 0});
        const auto n = point_.voltage_volt().size();
        SMatrix y{n, std::vector<Complex>(n * n)};
        for (std::size_t k = 0; k < n * n; ++k) {
            y.values[k] = frozen_detail::narrow(
                {point_.conductance_siemens()[k],
                 frozen_detail::real_product(omega, point_.capacitance_farad()[k])});
        }
        return y;
    }

    SMatrix external_y(double frequency_hz, const std::vector<DcNode> &external_nodes) const {
        const auto n = point_.voltage_volt().size();
        if (external_nodes.empty()) {
            throw std::invalid_argument("no external small-signal nodes");
        }
        std::vector<bool> selected(n, false);
        std::vector<std::size_t> e, internal;
        for (auto node : external_nodes) {
            if (!node || node > n || selected[node - 1]) {
                throw std::invalid_argument(
                    "external nodes must be distinct non-ground nodes in range");
            }
            selected[node - 1] = true;
            e.push_back(node - 1);
        }
        for (std::size_t k = 0; k < n; ++k) {
            if (!selected[k]) {
                internal.push_back(k);
            }
        }
        const auto y = nodal_y(frequency_hz);
        const auto p = e.size(), m = internal.size();
        SMatrix reduced{p, std::vector<Complex>(p * p)};
        for (std::size_t r = 0; r < p; ++r) {
            for (std::size_t c = 0; c < p; ++c) {
                reduced(r, c) = y(e[r], e[c]);
            }
        }
        if (!m) {
            return reduced;
        }
        std::vector<frozen_detail::Wide> a(m * m), b(m * p);
        for (std::size_t r = 0; r < m; ++r) {
            for (std::size_t c = 0; c < m; ++c) {
                a[r * m + c] = y(internal[r], internal[c]);
            }
            for (std::size_t c = 0; c < p; ++c) {
                b[r * p + c] = y(internal[r], e[c]);
            }
        }
        const auto x = frozen_detail::solve(std::move(a), std::move(b), m, p);
        for (std::size_t r = 0; r < p; ++r) {
            for (std::size_t c = 0; c < p; ++c) {
                frozen_detail::Wide value = reduced(r, c);
                for (std::size_t k = 0; k < m; ++k) {
                    value = frozen_detail::checked(
                        value - frozen_detail::product(frozen_detail::Wide(y(e[r], internal[k])),
                                                       x[k * p + c]));
                }
                reduced(r, c) = frozen_detail::narrow(value);
            }
        }
        return reduced;
    }

    SMatrix external_s(double f,
                       const std::vector<DcNode> &nodes,
                       const std::vector<Complex> &references_ohm) const {
        return admittance_to_power_wave_s(external_y(f, nodes), references_ohm);
    }

    // Adapter ports are 0..nodes.size()-1, not physical DC node IDs.
    // No noise model is inferred or supplied by this adapter.
    LinearNetwork linear_network(double f,
                                 const std::vector<DcNode> &nodes,
                                 const std::vector<Complex> &references_ohm) const {
        LinearNetwork result;
        result.add_power_waves(external_s(f, nodes, references_ohm), references_ohm);
        return result;
    }
};

inline FrozenSmallSignal freeze_small_signal(const DcOperatingPoint &point) {
    return FrozenSmallSignal(point);
}
} // namespace rfmodel
