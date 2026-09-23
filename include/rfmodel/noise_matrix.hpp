#pragma once
#include "device_model.hpp"
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

// Pivoted semidefinite Cholesky; return a PSD reconstruction. Roundoff below
// the tolerance is discarded, including a final numerically zero block.
inline SMatrix psd_matrix(const SMatrix &input, double scale_floor = 0.) {
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
        return input;
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
    SMatrix result{n, std::vector<Complex>(n * n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            Complex value = 0.;
            for (std::size_t k = 0; k < n; ++k) {
                value += factor(i, k) * std::conj(factor(j, k));
            }
            result(order[i], order[j]) = value * scale;
        }
    }
    finite_matrix(result);
    return result;
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
    auto c = noise_detail::psd_matrix(source.watts_per_hz);
    if (transfer.ports != c.ports) {
        throw std::invalid_argument("noise transfer dimensions differ");
    }
    const auto n = c.ports;
    SMatrix intermediate{n, std::vector<Complex>(n * n)}, result = intermediate;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            for (std::size_t k = 0; k < n; ++k) {
                intermediate(i, j) += transfer(i, k) * c(k, j);
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            for (std::size_t k = 0; k < n; ++k) {
                result(i, j) += intermediate(i, k) * std::conj(transfer(j, k));
            }
        }
    }
    return {noise_detail::psd_matrix(result)};
}
} // namespace rfmodel
