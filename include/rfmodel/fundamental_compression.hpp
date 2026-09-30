#pragma once
#include "device_model.hpp"
#include <cmath>
#include <limits>

namespace rfmodel {
// Fundamental response with optional total RF drive. No harmonic generation or saturation.
class P1dBFundamentalCompression {
    double amplitude_gain_;
    double input_p1db_w_;

public:
    P1dBFundamentalCompression(double power_gain_db, double output_p1db_dbm) {
        if (!std::isfinite(power_gain_db) || !std::isfinite(output_p1db_dbm)) {
            throw std::invalid_argument("nonfinite compression parameters");
        }
        amplitude_gain_ = std::pow(10., power_gain_db / 20.);
        // OP1dB is actual compressed output, so IP1dB = OP1dB - G + 1 dB.
        input_p1db_w_ = std::pow(10., (output_p1db_dbm - power_gain_db + 1. - 30.) / 10.);
        if (!std::isfinite(amplitude_gain_) || amplitude_gain_ <= 0 ||
            !std::isfinite(input_p1db_w_) || input_p1db_w_ <= 0) {
            throw std::invalid_argument("compression parameter range exceeds representation");
        }
    }

    double input_p1db_watts() const noexcept {
        return input_p1db_w_;
    }

    Complex transmit_fundamental(Complex incident) const {
        return transmit_fundamental(incident, std::norm(incident));
    }

    // The caller supplies total incident RF power, including this fundamental.
    // It may include other frequencies/ports; their waves must be solved separately.
    Complex transmit_fundamental(Complex incident, double total_incident_power_w) const {
        if (!std::isfinite(incident.real()) || !std::isfinite(incident.imag())) {
            throw std::invalid_argument("nonfinite incident fundamental");
        }
        const double power = std::norm(incident);
        if (!std::isfinite(total_incident_power_w) || total_incident_power_w < 0 ||
            !std::isfinite(power) ||
            (power > total_incident_power_w &&
             power - total_incident_power_w >
                 16 * std::numeric_limits<double>::epsilon() * power)) {
            throw std::invalid_argument("total RF drive must include the finite fundamental power");
        }
        const double ratio = total_incident_power_w / input_p1db_w_;
        if (!std::isfinite(ratio) || ratio > 1. + 16 * std::numeric_limits<double>::epsilon()) {
            throw std::out_of_range("fundamental compression input exceeds P1dB");
        }
        const double reduction = 1. - (1. - std::pow(10., -1. / 20.)) * ratio;
        const auto output = incident * reduction * amplitude_gain_;
        if (!std::isfinite(output.real()) || !std::isfinite(output.imag()) ||
            !std::isfinite(std::norm(output))) {
            throw std::overflow_error("compressed fundamental overflow");
        }
        return output;
    }
};
} // namespace rfmodel
