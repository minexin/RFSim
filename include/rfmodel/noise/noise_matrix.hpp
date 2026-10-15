#pragma once
#include "rfmodel/core/device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
// C(i,j)=E[c_i*conj(c_j)] per Hz, in W/Hz for power-normalized waves.
struct NoiseCorrelation {
    SMatrix watts_per_hz;
};

class NoiseCorrelationProvider {
public:
    virtual ~NoiseCorrelationProvider() = default;
    virtual NoiseCorrelation noise_correlation(double frequency_hz) const = 0;
};

namespace noise_detail {
inline void finite_matrix(const SMatrix &m) {
    if (!m.ports || m.ports > 1024 || m.values.size() != m.ports * m.ports) {
        throw std::invalid_argument("invalid noise matrix dimensions");
    }
    for (auto x : m.values) {
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            throw std::invalid_argument("nonfinite noise matrix");
        }
    }
}

// Normalized factor in original port order: C = scale * L * L^H.
// Keep the admitted rank so propagation never reconstructs C before applying H.
struct PsdFactor {
    SMatrix values;
    double scale;
    std::size_t rank;
};

// Pivoted semidefinite Cholesky. Input admission and discarded tiny modes use
// the same tolerance as psd_matrix, including the final numerically zero block.
inline PsdFactor legacy_psd_factor(const SMatrix &input, double scale_floor = 0.) {
    finite_matrix(input);
    const auto n = input.ports;
    double scale = scale_floor;
    for (auto x : input.values) {
        scale = std::max(scale, std::abs(x));
    }
    if (!std::isfinite(scale)) {
        throw std::overflow_error("noise scale overflow");
    }
    if (scale == 0) {
        return {input, 0., 0};
    }
    const double tolerance = 256 * n * std::numeric_limits<double>::epsilon();
    SMatrix work = input, factor{n, std::vector<Complex>(n * n)};
    for (auto &x : work.values) {
        x /= scale;
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            if (std::abs(work(i, j) - std::conj(work(j, i))) > tolerance) {
                throw std::invalid_argument("noise matrix is not Hermitian");
            }
        }
    }
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::size_t rank = 0;
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t pivot = k;
        for (std::size_t i = k + 1; i < n; ++i) {
            if (work(i, i).real() > work(pivot, pivot).real()) {
                pivot = i;
            }
        }
        if (work(pivot, pivot).real() <= tolerance) {
            for (std::size_t i = k; i < n; ++i) {
                for (std::size_t j = k; j < n; ++j) {
                    if (std::abs(work(i, j)) > tolerance) {
                        throw std::invalid_argument("noise matrix is not positive semidefinite");
                    }
                }
            }
            break;
        }
        for (std::size_t j = 0; j < n; ++j) {
            std::swap(work(k, j), work(pivot, j));
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::swap(work(i, k), work(i, pivot));
        }
        for (std::size_t j = 0; j < k; ++j) {
            std::swap(factor(k, j), factor(pivot, j));
        }
        std::swap(order[k], order[pivot]);
        ++rank;
        factor(k, k) = std::sqrt(work(k, k).real());
        for (std::size_t i = k + 1; i < n; ++i) {
            factor(i, k) = work(i, k) / factor(k, k);
        }
        for (std::size_t i = k + 1; i < n; ++i) {
            for (std::size_t j = k + 1; j < n; ++j) {
                work(i, j) -= factor(i, k) * std::conj(factor(j, k));
            }
        }
    }
    SMatrix original{n, std::vector<Complex>(n * n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < rank; ++k) {
            original(order[i], k) = factor(i, k);
        }
    }
    finite_matrix(original);
    return {std::move(original), scale, rank};
}

// Keep the global admission/projection above unchanged. The retention contract
// additionally preserves wholly discarded, exact-independent input components
// when their local projection fits the old common-scale representation. This is
// not recovery of weak modes within a partially retained connected component.
inline PsdFactor psd_factor(const SMatrix &input, double scale_floor = 0.) {
    auto factor = legacy_psd_factor(input, scale_floor);
    const auto n = input.ports;
    if (scale_floor != 0. || factor.scale == 0. || factor.rank == n) {
        return factor;
    }
    const auto original_rank = factor.rank;
    std::vector<bool> visited(n);
    for (std::size_t seed = 0; seed < n; ++seed) {
        if (visited[seed]) {
            continue;
        }
        std::vector<std::size_t> rows{seed};
        visited[seed] = true;
        for (std::size_t cursor = 0; cursor < rows.size(); ++cursor) {
            const auto i = rows[cursor];
            for (std::size_t j = 0; j < n; ++j) {
                // Use both original directions, including globally tolerated
                // non-Hermitian entries. A numerical-zero threshold is unsafe.
                if (!visited[j] && (input(i, j) != Complex{} || input(j, i) != Complex{})) {
                    visited[j] = true;
                    rows.push_back(j);
                }
            }
        }
        bool wholly_discarded = true;
        for (auto i : rows) {
            for (std::size_t k = 0; k < original_rank; ++k) {
                wholly_discarded = wholly_discarded && factor.values(i, k) == Complex{};
            }
        }
        if (!wholly_discarded) {
            continue;
        }
        std::sort(rows.begin(), rows.end());
        const auto size = rows.size();
        SMatrix component{size, std::vector<Complex>(size * size)};
        bool normal_parts = true;
        for (std::size_t i = 0; i < size; ++i) {
            for (std::size_t j = 0; j < size; ++j) {
                const auto value = input(rows[i], rows[j]);
                component(i, j) = value;
                for (double part : {value.real(), value.imag()}) {
                    if (part != 0. && !std::isnormal(part / factor.scale)) {
                        normal_parts = false;
                    }
                }
            }
        }
        if (!normal_parts) {
            continue;
        }
        try {
            // One local legacy attempt, never recursive component recovery.
            auto extra = legacy_psd_factor(component);
            if (extra.rank == 0) {
                continue;
            }
            const double ratio = extra.scale / factor.scale;
            if (!std::isnormal(ratio)) {
                continue;
            }
            const double root = std::sqrt(ratio);
            if (!std::isnormal(root)) {
                continue;
            }
            bool normal_factor = true;
            for (std::size_t i = 0; i < size; ++i) {
                for (std::size_t k = 0; k < extra.rank; ++k) {
                    const auto value = extra.values(i, k);
                    const auto scaled = value * root;
                    if ((value.real() != 0. && !std::isnormal(scaled.real())) ||
                        (value.imag() != 0. && !std::isnormal(scaled.imag()))) {
                        normal_factor = false;
                    }
                    extra.values(i, k) = scaled;
                }
            }
            if (!normal_factor) {
                continue; // Never append only part of a local component.
            }
            // These columns have support only on previously zero rows, so their
            // total rank fits the existing n-by-n storage. Old coefficients and
            // scale remain bitwise unchanged. Stage all scaling before appending:
            // local fill-in can underflow despite normal original input parts.
            for (std::size_t i = 0; i < size; ++i) {
                for (std::size_t k = 0; k < extra.rank; ++k) {
                    factor.values(rows[i], factor.rank + k) = extra.values(i, k);
                }
            }
            factor.rank += extra.rank;
        } catch (const std::invalid_argument &) {
            // Local Hermitian/PSD failure retains the admitted global projection.
        }
    }
    finite_matrix(factor.values);
    return factor;
}

inline SMatrix psd_matrix(const SMatrix &input, double scale_floor = 0.) {
    const auto factor = psd_factor(input, scale_floor);
    const auto n = input.ports;
    SMatrix result{n, std::vector<Complex>(n * n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            Complex value = 0.;
            for (std::size_t k = 0; k < factor.rank; ++k) {
                value += factor.values(i, k) * std::conj(factor.values(j, k));
            }
            result(i, j) = value * factor.scale;
        }
    }
    finite_matrix(result);
    return result;
}

// Binary-scaled floating expansion. Keeping nonoverlapping residuals prevents
// large cancelling terms from erasing a small surviving term. Products and sums
// never need a wider long double range, including on MSVC.
struct ScaledReal {
    double value = 0.;
    int exponent = 0;
};

inline ScaledReal normalized(double value, int exponent = 0) {
    int shift = 0;
    const auto fraction = std::frexp(value, &shift);
    return {fraction, value == 0. ? 0 : exponent + shift};
}

class ScaledSum {
    std::vector<ScaledReal> partials_;

public:
    void add(double value, int exponent) {
        auto x = normalized(value, exponent);
        if (x.value == 0.) {
            return;
        }
        std::size_t count = 0;
        for (auto y : partials_) {
            if (x.value == 0.) {
                x = y;
                continue;
            }
            if (x.exponent < y.exponent ||
                (x.exponent == y.exponent && std::abs(x.value) < std::abs(y.value))) {
                std::swap(x, y);
            }
            const int difference = x.exponent - y.exponent;
            if (difference > 53) {
                // The smaller operand cannot change the larger double, but
                // must survive in case later terms cancel the larger operand.
                partials_[count++] = y;
                continue;
            }
            const double small = std::scalbn(y.value, -difference);
            const double high = x.value + small;
            const double low = small - (high - x.value); // FastTwoSum
            if (low != 0.) {
                partials_[count++] = normalized(low, x.exponent);
            }
            x = normalized(high, x.exponent);
        }
        partials_.resize(count);
        if (x.value != 0.) {
            partials_.push_back(x);
        }
    }

    void product(ScaledReal a, ScaledReal b, double sign = 1.) {
        if (a.value == 0. || b.value == 0.) {
            return;
        }
        const double high = a.value * b.value;
        const double low = std::fma(a.value, b.value, -high);
        add(sign * high, a.exponent + b.exponent);
        add(sign * low, a.exponent + b.exponent);
    }

    ScaledReal value() const {
        if (partials_.empty()) {
            return {};
        }
        const int exponent = partials_.back().exponent;
        double value = 0.;
        for (const auto &p : partials_) {
            value += std::scalbn(p.value, p.exponent - exponent);
        }
        return normalized(value, exponent);
    }
};

// Compensated ordinary-range path. Difficult exponent ranges or severe
// cancellation use the scaled expansion instead. Product residuals are kept so
// the fast path does not turn cancellation into a covariance-scale error.
class OrdinarySum {
    double sum_ = 0., correction_ = 0., magnitude_ = 0.;
    bool safe_ = true;

public:
    void product(double a, double b, double sign = 1.) {
        if (a == 0. || b == 0.) {
            return;
        }
        const double product = a * b;
        // Headroom also keeps the FMA residual (up to 106 significand
        // bits below a product) representable and dense sums finite.
        if (!std::isfinite(product) || std::abs(product) < 0x1p-900 ||
            std::abs(product) > 0x1p900) {
            safe_ = false;
            return;
        }
        const double term = sign * product;
        const double next = sum_ + term;
        correction_ +=
            (std::abs(sum_) >= std::abs(term) ? (sum_ - next) + term : (term - next) + sum_) +
            sign * std::fma(a, b, -product);
        sum_ = next;
        magnitude_ += std::abs(term);
    }

    bool finish(double &value) const {
        value = sum_ + correction_;
        return safe_ && std::isfinite(value) && std::isfinite(magnitude_) &&
               (magnitude_ == 0. || (std::abs(value) >= 1e-8 * magnitude_ &&
                                     std::abs(value) >= std::numeric_limits<double>::min()));
    }
};

inline void ordinary_product(
    OrdinarySum &real, OrdinarySum &imag, Complex a, Complex b, bool conjugate = false) {
    real.product(a.real(), b.real());
    real.product(a.imag(), b.imag(), conjugate ? 1. : -1.);
    imag.product(a.imag(), b.real());
    imag.product(a.real(), b.imag(), conjugate ? -1. : 1.);
}

struct ScaledComplex {
    ScaledReal real, imag;
};

inline ScaledComplex scaled(Complex value) {
    return {normalized(value.real()), normalized(value.imag())};
}

inline void complex_product(ScaledSum &real,
                            ScaledSum &imag,
                            const ScaledComplex &a,
                            const ScaledComplex &b,
                            bool conjugate = false) {
    real.product(a.real, b.real);
    real.product(a.imag, b.imag, conjugate ? 1. : -1.);
    imag.product(a.imag, b.real);
    imag.product(a.real, b.imag, conjugate ? -1. : 1.);
}

inline double physical_value(ScaledReal value, double scale) {
    const auto source = normalized(scale);
    return std::scalbn(value.value * source.value, value.exponent + source.exponent);
}
} // namespace noise_detail

inline NoiseCorrelation passive_thermal_noise(const SMatrix &s, double temperature_k) {
    noise_detail::finite_matrix(s);
    if (!std::isfinite(temperature_k) || temperature_k < 0) {
        throw std::invalid_argument("invalid physical temperature");
    }
    SMatrix defect{s.ports, std::vector<Complex>(s.values.size())};
    for (std::size_t i = 0; i < s.ports; ++i) {
        for (std::size_t j = 0; j < s.ports; ++j) {
            Complex value = i == j ? 1. : 0.;
            for (std::size_t k = 0; k < s.ports; ++k) {
                value -= s(i, k) * std::conj(s(j, k));
            }
            defect(i, j) = value;
        }
    }
    // Check passivity even at zero temperature.
    auto covariance = noise_detail::psd_matrix(defect, 1.);
    constexpr double boltzmann = 1.380649e-23;
    for (auto &x : covariance.values) {
        x *= boltzmann * temperature_k;
    }
    noise_detail::finite_matrix(covariance);
    return {std::move(covariance)};
}

// Blocks follow device insertion order. Cross-device correlations are zero.
inline NoiseCorrelation independent_noise(const std::vector<NoiseCorrelation> &blocks) {
    if (blocks.empty()) {
        throw std::invalid_argument("empty independent noise blocks");
    }
    std::size_t total = 0;
    for (const auto &block : blocks) {
        noise_detail::finite_matrix(block.watts_per_hz);
        total += block.watts_per_hz.ports;
        if (total > 1024) {
            throw std::invalid_argument("noise port limit exceeded");
        }
    }
    SMatrix result{total, std::vector<Complex>(total * total)};
    std::size_t offset = 0;
    for (const auto &block : blocks) {
        const auto c = noise_detail::psd_matrix(block.watts_per_hz);
        for (std::size_t i = 0; i < c.ports; ++i) {
            for (std::size_t j = 0; j < c.ports; ++j) {
                result(offset + i, offset + j) = c(i, j);
            }
        }
        offset += c.ports;
    }
    return {std::move(result)};
}

// Square transfer map: outgoing noise = transfer * source noise.
inline NoiseCorrelation propagate_noise(const SMatrix &transfer, const NoiseCorrelation &source) {
    noise_detail::finite_matrix(transfer);
    auto factor = noise_detail::psd_factor(source.watts_per_hz);
    const auto n = factor.values.ports;
    if (transfer.ports != n) {
        throw std::invalid_argument("noise transfer dimensions differ");
    }
    std::vector<noise_detail::ScaledComplex> propagated(n * factor.rank);
    std::vector<Complex> ordinary(n * factor.rank);
    std::vector<bool> bounded(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < factor.rank; ++j) {
            noise_detail::OrdinarySum fast_real, fast_imag;
            for (std::size_t k = 0; k < n; ++k) {
                noise_detail::ordinary_product(
                    fast_real, fast_imag, transfer(i, k), factor.values(k, j));
            }
            double real_value = 0., imag_value = 0.;
            auto &entry = propagated[i * factor.rank + j];
            if (fast_real.finish(real_value) && fast_imag.finish(imag_value)) {
                entry = noise_detail::scaled({real_value, imag_value});
            } else {
                noise_detail::ScaledSum real, imag;
                for (std::size_t k = 0; k < n; ++k) {
                    noise_detail::complex_product(real,
                                                  imag,
                                                  noise_detail::scaled(transfer(i, k)),
                                                  noise_detail::scaled(factor.values(k, j)));
                }
                entry = {real.value(), imag.value()};
            }
            const bool safe = (entry.real.value == 0. ||
                               (entry.real.exponent >= -400 && entry.real.exponent <= 400)) &&
                              (entry.imag.value == 0. ||
                               (entry.imag.exponent >= -400 && entry.imag.exponent <= 400));
            bounded[i] = bounded[i] && safe;
            if (safe) {
                ordinary[i * factor.rank + j] = {
                    std::scalbn(entry.real.value, entry.real.exponent),
                    std::scalbn(entry.imag.value, entry.imag.exponent)};
            }
        }
    }
    SMatrix result{n, std::vector<Complex>(n * n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            noise_detail::ScaledReal real_value, imag_value;
            bool finished = false;
            if (bounded[i] && bounded[j]) {
                noise_detail::OrdinarySum real, imag;
                for (std::size_t k = 0; k < factor.rank; ++k) {
                    noise_detail::ordinary_product(real,
                                                   imag,
                                                   ordinary[i * factor.rank + k],
                                                   ordinary[j * factor.rank + k],
                                                   true);
                }
                double re = 0., im = 0.;
                finished = real.finish(re) && (i == j || imag.finish(im));
                if (finished) {
                    real_value = noise_detail::normalized(re);
                    imag_value = noise_detail::normalized(im);
                }
            }
            if (!finished) {
                noise_detail::ScaledSum real, imag;
                for (std::size_t k = 0; k < factor.rank; ++k) {
                    noise_detail::complex_product(real,
                                                  imag,
                                                  propagated[i * factor.rank + k],
                                                  propagated[j * factor.rank + k],
                                                  true);
                }
                real_value = real.value();
                imag_value = imag.value();
            }
            const Complex value{noise_detail::physical_value(real_value, factor.scale),
                                i == j ? 0.
                                       : noise_detail::physical_value(imag_value, factor.scale)};
            result(i, j) = value;
            result(j, i) = std::conj(value);
        }
    }
    noise_detail::finite_matrix(result);
    return {std::move(result)};
}
} // namespace rfmodel
