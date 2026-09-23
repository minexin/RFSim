#pragma once
#include "touchstone.hpp"
#include <algorithm>

namespace rfmodel {
enum class OutOfBand {
    Reject,
    Clamp
};

inline void validate_s_data(const TouchstoneData &d) {
    if (!d.ports || d.ports > 1024 || d.matrices.empty() ||
        d.frequencies_hz.size() != d.matrices.size()) {
        throw std::invalid_argument("inconsistent S parameter dataset");
    }
    if (!std::isfinite(d.reference_impedance_ohms) || d.reference_impedance_ohms <= 0) {
        throw std::invalid_argument("invalid reference resistance");
    }
    for (std::size_t i = 0; i < d.matrices.size(); ++i) {
        const double f = d.frequencies_hz[i];
        if (!std::isfinite(f) || f < 0 || (i && f <= d.frequencies_hz[i - 1])) {
            throw std::invalid_argument("invalid frequency grid");
        }
        const auto &m = d.matrices[i];
        if (m.ports != d.ports || m.values.size() != d.ports * d.ports) {
            throw std::invalid_argument("inconsistent S matrix dimensions");
        }
        for (auto x : m.values) {
            if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
                throw std::invalid_argument("nonfinite S parameter");
            }
        }
    }
}

inline SMatrix
interpolate_s(const TouchstoneData &d, double f, OutOfBand policy = OutOfBand::Reject) {
    validate_s_data(d);
    if (!std::isfinite(f) || f < 0) {
        throw std::invalid_argument("invalid query frequency");
    }
    if (f < d.frequencies_hz.front() || f > d.frequencies_hz.back()) {
        if (policy == OutOfBand::Reject) {
            throw std::out_of_range("frequency outside model range");
        }
        return f < d.frequencies_hz.front() ? d.matrices.front() : d.matrices.back();
    }
    const auto it = std::lower_bound(d.frequencies_hz.begin(), d.frequencies_hz.end(), f);
    const auto i = static_cast<std::size_t>(it - d.frequencies_hz.begin());
    if (*it == f) {
        return d.matrices[i];
    }
    const double t =
        (f - d.frequencies_hz[i - 1]) / (d.frequencies_hz[i] - d.frequencies_hz[i - 1]);
    SMatrix result{d.ports, std::vector<Complex>(d.ports * d.ports)};
    for (std::size_t k = 0; k < result.values.size(); ++k) {
        result.values[k] = d.matrices[i - 1].values[k] * (1 - t) + d.matrices[i].values[k] * t;
    }
    return result;
}
} // namespace rfmodel
