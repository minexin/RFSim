#pragma once
#include "fundamental_compression.hpp"

namespace rfmodel {
// Fundamental-only cubic / incremental-tanh response. No harmonics or AM/PM.
class SaturatingFundamentalCompression {
    P1dBFundamentalCompression below_p1db_;
    double input_anchor_;
    double output_anchor_;
    double output_headroom_;
    double amplitude_slope_;

public:
    SaturatingFundamentalCompression(double power_gain_db,
                                     double output_p1db_dbm,
                                     double output_saturation_dbm)
        : below_p1db_(power_gain_db, output_p1db_dbm) {
        if (!std::isfinite(output_saturation_dbm) || output_saturation_dbm <= output_p1db_dbm) {
            throw std::invalid_argument("saturation must exceed output P1dB");
        }
        const double output_p1db_w = std::pow(10., (output_p1db_dbm - 30.) / 10.);
        const double saturation_w = std::pow(10., (output_saturation_dbm - 30.) / 10.);
        if (!std::isfinite(output_p1db_w) || output_p1db_w <= 0 || !std::isfinite(saturation_w) ||
            saturation_w <= output_p1db_w) {
            throw std::invalid_argument("saturation anchors exceed power representation");
        }
        input_anchor_ = std::sqrt(below_p1db_.input_p1db_watts());
        output_anchor_ = std::sqrt(output_p1db_w);
        output_headroom_ = std::sqrt(saturation_w) - output_anchor_;
        amplitude_slope_ = std::pow(10., power_gain_db / 20.) * (3 * std::pow(10., -.05) - 2);
        if (output_headroom_ <= 0) {
            throw std::invalid_argument("saturation headroom is not representable");
        }
    }

    double input_p1db_watts() const noexcept {
        return below_p1db_.input_p1db_watts();
    }

    // Apply this same gain to all source contributions AFTER solving physical
    // coherent drive. Individual cancelling contributions may exceed that drive.
    double amplitude_gain(double total_incident_power_w) const {
        if (!std::isfinite(total_incident_power_w) || total_incident_power_w < 0) {
            throw std::invalid_argument("invalid total RF drive");
        }
        if (total_incident_power_w <= input_p1db_watts()) {
            return below_p1db_.amplitude_gain(total_incident_power_w);
        }
        const double drive = std::sqrt(total_incident_power_w);
        const double argument = amplitude_slope_ * ((drive - input_anchor_) / output_headroom_);
        const double amplitude = output_anchor_ + output_headroom_ * std::tanh(argument);
        const double gain = amplitude / drive;
        if (!std::isfinite(gain)) {
            throw std::overflow_error("saturated amplitude gain overflow");
        }
        return gain;
    }

    Complex transmit_fundamental(Complex incident) const {
        return transmit_fundamental(incident, std::norm(incident));
    }

    // Total includes the selected fundamental. It is externally solved, not inferred here.
    Complex transmit_fundamental(Complex incident, double total_incident_power_w) const {
        const double power = std::norm(incident);
        if (!std::isfinite(incident.real()) || !std::isfinite(incident.imag()) ||
            !std::isfinite(power) || !std::isfinite(total_incident_power_w) ||
            total_incident_power_w < 0 ||
            (power > total_incident_power_w &&
             power - total_incident_power_w >
                 16 * std::numeric_limits<double>::epsilon() * power)) {
            throw std::invalid_argument("total RF drive must include the finite fundamental power");
        }
        if (total_incident_power_w <= input_p1db_watts()) {
            return below_p1db_.transmit_fundamental(incident, total_incident_power_w);
        }
        const double drive = std::sqrt(total_incident_power_w);
        // A positive overflowing argument intentionally reaches tanh(+inf) = 1.
        const double argument = amplitude_slope_ * ((drive - input_anchor_) / output_headroom_);
        const double amplitude = output_anchor_ + output_headroom_ * std::tanh(argument);
        const auto output = (incident / drive) * amplitude;
        if (!std::isfinite(output.real()) || !std::isfinite(output.imag()) ||
            !std::isfinite(std::norm(output))) {
            throw std::overflow_error("saturated fundamental overflow");
        }
        return output;
    }
};
} // namespace rfmodel
