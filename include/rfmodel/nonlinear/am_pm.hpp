#pragma once
#include "rfmodel/core/device_model.hpp"
#include <cmath>

namespace rfmodel {
struct AmPmResponse {
    double phase_rad;
    // d(phi)/d(log amplitude) = 2*P*d(phi)/dP; finite at zero drive.
    double radial_phase_slope;
};

// Legacy point-derivative AM/PM: the specified degrees/dB slope peaks at
// output_reference_dbm. The low-power phase is zero; high-power phase is flat.
// Derived from the documented amplitude-domain cubic and its dB peak scaling.
class LegacyPointDerivativeAmPm {
    double reference_radius_, phase_scale_;

public:
    LegacyPointDerivativeAmPm(double peak_degrees_per_db, double output_reference_dbm) {
        if (!std::isfinite(peak_degrees_per_db) || !std::isfinite(output_reference_dbm)) {
            throw std::invalid_argument("nonfinite AM-PM parameters");
        }
        const double power = std::pow(10., (output_reference_dbm - 30.) / 10.);
        if (!std::isfinite(power) || power <= 0.) {
            throw std::invalid_argument("AM-PM reference power exceeds representation");
        }
        reference_radius_ = std::sqrt(power);
        phase_scale_ = (peak_degrees_per_db * (std::acos(-1.) / 180.)) / std::log(10.);
    }

    AmPmResponse response(double output_power_w) const {
        if (!std::isfinite(output_power_w) || output_power_w < 0.) {
            throw std::invalid_argument("invalid AM-PM output power");
        }
        const double radius = std::sqrt(output_power_w);
        // Compare radii before division to avoid overflow for tiny references.
        if (radius >= 1.5 * reference_radius_) {
            return {22.5 * phase_scale_, 0.};
        }
        const double u = radius / reference_radius_, square = u * u;
        return {phase_scale_ * square * (30. - (40. / 3.) * u),
                (20. * phase_scale_) * square * (3. - 2. * u)};
    }

    Complex transmit(Complex outgoing) const {
        if (!std::isfinite(outgoing.real()) || !std::isfinite(outgoing.imag())) {
            throw std::invalid_argument("nonfinite AM-PM output wave");
        }
        return std::polar(1., response(std::norm(outgoing)).phase_rad) * outgoing;
    }
};

// Opt-in empirical RFAMP PointDerivative mapping for SystemVue 2023.0.0.11903.
// The constants are frozen calibration data, not a derivation of vendor code.
// See docs/systemvue-am-pm-adapter.md for observed scope and residuals.
class SystemVue2023PointDerivativeAmPm {
    LegacyPointDerivativeAmPm phase_;

public:
    static constexpr double peak_scale = 1.0000010001140027;
    static constexpr double reference_shift_db = -.0012252678340014533;

    SystemVue2023PointDerivativeAmPm(double peak_degrees_per_db, double output_reference_dbm)
        : phase_(peak_degrees_per_db * peak_scale, output_reference_dbm + reference_shift_db) {
    }

    AmPmResponse response(double output_power_w) const {
        return phase_.response(output_power_w);
    }

    Complex transmit(Complex outgoing) const {
        return phase_.transmit(outgoing);
    }
};
} // namespace rfmodel
