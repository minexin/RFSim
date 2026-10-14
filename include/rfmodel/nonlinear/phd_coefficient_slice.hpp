#pragma once
#include "rfmodel/conversion/frequency_conversion.hpp"
#include <string>
#include <utility>

namespace rfmodel {

// These are native RMS power-wave coefficients, not vendor pseudowave data.
enum class PhdCoefficientConvention {
    native_rms_power_wave_v1
};

struct PhdCoefficientData {
    PhdCoefficientConvention convention{PhdCoefficientConvention::native_rms_power_wave_v1};
    double fundamental_hz{};
    std::vector<double> port_reference_ohms;
    std::vector<std::string> port_reference_planes;
    std::vector<ConversionChannel> channels;
    std::size_t pump_channel{};
    double pump_magnitude_sqrt_w{};
    std::vector<Complex> baseline_outgoing;
    SMatrix direct;
    SMatrix conjugate;
    std::string provenance;
};

namespace phd_detail {
inline void validate_delta(const std::vector<Complex> &delta,
                           const std::vector<ConversionChannel> &channels,
                           std::size_t pump) {
    if (delta.size() != channels.size()) {
        throw std::invalid_argument("PHD perturbation dimensions differ");
    }
    for (std::size_t i = 0; i < delta.size(); ++i) {
        if (!conversion_detail::finite(delta[i]) ||
            (channels[i].bin == 0 && delta[i].imag() != 0.)) {
            throw std::invalid_argument("PHD perturbations must be finite and DC real");
        }
    }
    if (delta[pump] != Complex{}) {
        throw std::invalid_argument("PHD pump is clamped: nonzero pump perturbation");
    }
}

inline Complex phase(double theta, double harmonic) {
    return std::polar(1., harmonic * theta);
}

inline void finite_wave(Complex value) {
    if (!conversion_detail::finite(value)) {
        throw std::overflow_error("PHD wave is not finite");
    }
}
} // namespace phd_detail

class PhdCoefficientSlice;
class PhdFrozenSlice;

// Deliberately not a FrequencyConversionModel: its domain excludes pump changes.
// Scalar entries describe the restricted tangent only, not a full Newton Jacobian.
class PhdClampedJacobian {
    std::vector<ConversionChannel> channels_;
    std::size_t pump_;
    SMatrix direct_, conjugate_;
    friend class PhdFrozenSlice;

    PhdClampedJacobian(std::vector<ConversionChannel> channels,
                       std::size_t pump,
                       SMatrix direct,
                       SMatrix conjugate)
        : channels_(std::move(channels)), pump_(pump), direct_(std::move(direct)),
          conjugate_(std::move(conjugate)) {
    }

public:
    std::size_t size() const {
        return channels_.size();
    }

    std::size_t pump_channel() const {
        return pump_;
    }

    Complex direct(std::size_t row, std::size_t column) const {
        if (row >= size() || column >= size()) {
            throw std::out_of_range("PHD Jacobian index");
        }
        return direct_(row, column);
    }

    Complex conjugate(std::size_t row, std::size_t column) const {
        if (row >= size() || column >= size()) {
            throw std::out_of_range("PHD Jacobian index");
        }
        return conjugate_(row, column);
    }

    std::vector<Complex> apply(const std::vector<Complex> &delta) const {
        phd_detail::validate_delta(delta, channels_, pump_);
        std::vector<Complex> result(size());
        for (std::size_t i = 0; i < size(); ++i) {
            for (std::size_t j = 0; j < size(); ++j) {
                result[i] += direct_(i, j) * delta[j] + conjugate_(i, j) * std::conj(delta[j]);
            }
            phd_detail::finite_wave(result[i]);
        }
        return result;
    }
};

class PhdFrozenSlice {
    PhdCoefficientData data_;
    double phase_;
    std::vector<Complex> incident_, outgoing_, offset_;
    PhdClampedJacobian jacobian_;
    friend class PhdCoefficientSlice;

    PhdFrozenSlice(PhdCoefficientData data, double phase)
        : data_(std::move(data)), phase_(phase), incident_(data_.channels.size()),
          outgoing_(data_.channels.size()), offset_(data_.channels.size()),
          jacobian_(data_.channels, data_.pump_channel, data_.direct, data_.conjugate) {
        if (!std::isfinite(phase_)) {
            throw std::invalid_argument("PHD phase must be finite");
        }
        phase_ = std::remainder(phase_, 2. * std::acos(-1.));
        incident_[data_.pump_channel] = data_.pump_magnitude_sqrt_w * phd_detail::phase(phase_, 1.);
        for (std::size_t i = 0; i < data_.channels.size(); ++i) {
            const double m = data_.channels[i].bin;
            outgoing_[i] = data_.baseline_outgoing[i] * phd_detail::phase(phase_, m);
            phd_detail::finite_wave(outgoing_[i]);
            for (std::size_t j = 0; j < data_.channels.size(); ++j) {
                const double n = data_.channels[j].bin;
                jacobian_.direct_(i, j) *= phd_detail::phase(phase_, m - n);
                jacobian_.conjugate_(i, j) *= phd_detail::phase(phase_, m + n);
                // Preserve exact real-DC structure despite floating-point phase roundoff.
                if (m == 0.) {
                    jacobian_.conjugate_(i, j) = std::conj(jacobian_.direct_(i, j));
                }
                phd_detail::finite_wave(jacobian_.direct_(i, j));
                phd_detail::finite_wave(jacobian_.conjugate_(i, j));
            }
            // d=b0-D*a0-C*conj(a0). Zero pump columns give d=b0 exactly.
            offset_[i] = outgoing_[i];
        }
    }

public:
    const PhdCoefficientData &coefficients() const {
        return data_;
    }

    double pump_phase_radians() const {
        return phase_;
    }

    const std::vector<Complex> &nominal_incident() const {
        return incident_;
    }

    const std::vector<Complex> &nominal_outgoing() const {
        return outgoing_;
    }

    const std::vector<Complex> &output_offset() const {
        return offset_;
    }

    const PhdClampedJacobian &perturbation_jacobian() const {
        return jacobian_;
    }

    std::vector<Complex> evaluate_perturbation(const std::vector<Complex> &delta) const {
        auto result = jacobian_.apply(delta);
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] += outgoing_[i];
            phd_detail::finite_wave(result[i]);
        }
        return result;
    }

    // Total-wave evaluation still requires the EXACT frozen pump coordinate.
    std::vector<Complex> evaluate_incident(std::vector<Complex> incident) const {
        if (incident.size() != incident_.size()) {
            throw std::invalid_argument("PHD incident dimensions differ");
        }
        for (std::size_t i = 0; i < incident.size(); ++i) {
            incident[i] -= incident_[i];
        }
        return evaluate_perturbation(incident);
    }
};

// One amplitude slice only. Changing amplitude requires independently supplied data.
class PhdCoefficientSlice {
    PhdCoefficientData data_;

public:
    explicit PhdCoefficientSlice(PhdCoefficientData data) : data_(std::move(data)) {
        const auto n = data_.channels.size(), ports = data_.port_reference_ohms.size();
        if (data_.convention != PhdCoefficientConvention::native_rms_power_wave_v1 || n == 0 ||
            n > 512 || ports == 0 || ports > 1024 || !std::isfinite(data_.fundamental_hz) ||
            data_.fundamental_hz <= 0. || !std::isfinite(data_.pump_magnitude_sqrt_w) ||
            data_.pump_magnitude_sqrt_w <= 0. || data_.pump_channel >= n ||
            data_.channels[data_.pump_channel].bin != 1 ||
            data_.port_reference_planes.size() != ports || data_.baseline_outgoing.size() != n) {
            throw std::invalid_argument("invalid native PHD slice metadata");
        }
        std::vector<bool> used(ports);
        for (std::size_t p = 0; p < ports; ++p) {
            if (!std::isfinite(data_.port_reference_ohms[p]) ||
                data_.port_reference_ohms[p] <= 0. || data_.port_reference_planes[p].empty()) {
                throw std::invalid_argument(
                    "PHD requires positive real port references and plane IDs");
            }
        }
        noise_detail::finite_matrix(data_.direct);
        noise_detail::finite_matrix(data_.conjugate);
        if (data_.direct.ports != n || data_.conjugate.ports != n) {
            throw std::invalid_argument("PHD coefficient dimensions differ");
        }
        std::set<std::pair<std::size_t, int>> seen;
        for (std::size_t i = 0; i < n; ++i) {
            const auto channel = data_.channels[i];
            if (channel.port >= ports || channel.bin < 0 ||
                !std::isfinite(channel.bin * data_.fundamental_hz) ||
                !seen.emplace(channel.port, channel.bin).second ||
                !conversion_detail::finite(data_.baseline_outgoing[i]) ||
                (channel.bin == 0 && data_.baseline_outgoing[i].imag() != 0.)) {
                throw std::invalid_argument("invalid PHD channel or baseline");
            }
            used[channel.port] = true;
            for (std::size_t j = 0; j < n; ++j) {
                const auto s = data_.direct(i, j), t = data_.conjugate(i, j);
                if ((j == data_.pump_channel && (s != Complex{} || t != Complex{})) ||
                    (data_.channels[j].bin == 0 && s != t) ||
                    (channel.bin == 0 && t != std::conj(s))) {
                    throw std::invalid_argument(
                        "PHD pump column or canonical real-DC gauge violated");
                }
            }
        }
        for (const auto present : used) {
            if (!present) {
                throw std::invalid_argument("PHD physical ports must be dense and represented");
            }
        }
    }

    const PhdCoefficientData &coefficients() const {
        return data_;
    }

    PhdFrozenSlice freeze_pump(double phase_radians) const {
        return PhdFrozenSlice(data_, phase_radians);
    }
};
} // namespace rfmodel
