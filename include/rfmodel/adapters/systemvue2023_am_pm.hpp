#pragma once
#include "rfmodel/nonlinear/point_derivative_am_pm.hpp"

namespace rfmodel {
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
