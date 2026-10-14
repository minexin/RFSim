#pragma once
#include "rfmodel/models/ideal_devices.hpp"
#include <algorithm>
#include <limits>

namespace rfmodel {
// Uniform reciprocal line with real characteristic impedance. Loss is the
// one-way propagation loss, not the insertion loss of the mismatched two-port.
class TransmissionLineModel final : public RFDeviceModel, public SParameterProvider {
    MatchedTransmissionModel propagation_;
    double reflection_;
    double coupling_;
    double delay_s_;
    double loss_db_;

public:
    TransmissionLineModel(std::string name,
                          double characteristic_ohms,
                          double delay_s,
                          double propagation_loss_db = 0.,
                          double reference_ohms = 50.)
        : propagation_(std::move(name), propagation_loss_db, delay_s, reference_ohms),
          delay_s_(delay_s), loss_db_(propagation_loss_db) {
        if (!std::isfinite(characteristic_ohms) || characteristic_ohms <= 0.) {
            throw std::invalid_argument("invalid line characteristic impedance");
        }
        const double scale = std::max(characteristic_ohms, reference_ohms);
        const double line = characteristic_ohms / scale;
        const double reference = reference_ohms / scale;
        reflection_ = (line - reference) / (line + reference);
        const double ratio = std::min(line, reference);
        coupling_ = 4. * ratio / ((1. + ratio) * (1. + ratio));
        if (1. - std::abs(reflection_) <= 64 * std::numeric_limits<double>::epsilon()) {
            throw std::invalid_argument("line impedance contrast exceeds numerical range");
        }
    }

    std::string name() const override {
        return propagation_.name();
    }

    std::size_t port_count() const override {
        return 2;
    }

    PortInfo port(std::size_t index) const override {
        return propagation_.port(index);
    }

    SMatrix s_parameters(double frequency_hz) const override {
        if (!std::isfinite(frequency_hz) || frequency_hz < 0.) {
            throw std::invalid_argument("invalid frequency");
        }
        const double cycles = frequency_hz * delay_s_;
        if (!std::isfinite(cycles)) {
            throw std::overflow_error("delay phase overflow");
        }
        constexpr double two_pi = 6.28318530717958647692;
        constexpr double db_to_nepers = 0.11512925464970228420;
        const double reduced = std::remainder(cycles, 1.);
        const int quadrant = static_cast<int>(std::round(4. * reduced));
        // Reduce in turns, before multiplying by pi: exact quarter/half turns
        // stay exact without snapping their representable neighbours to them.
        const double phase = -two_pi * (reduced - 0.25 * quadrant);
        double sine = std::sin(phase);
        double cosine = std::cos(phase);
        const double original_sine = sine;
        if (quadrant == 1) {
            sine = -cosine;
            cosine = original_sine;
        } else if (quadrant == -1) {
            sine = cosine;
            cosine = -original_sine;
        } else if (quadrant == 2 || quadrant == -2) {
            sine = -sine;
            cosine = -cosine;
        }
        const double alpha = loss_db_ * db_to_nepers;
        const double magnitude = std::exp(-alpha);
        const double round_trip_magnitude = std::exp(-2. * alpha);
        // Normalize before multiplying tiny loss by the dB conversion factor:
        // even denorm_min loss can produce a representable mismatched response.
        // The omitted relative correction here is O(alpha), far below epsilon.
        const double attenuation_defect = alpha < 1e-100
                                              ? (loss_db_ / coupling_) * (2. * db_to_nepers)
                                              : -std::expm1(-2. * alpha) / coupling_;
        double normalized_sine = sine / coupling_;
        if (cycles < 1e-100) {
            // The product may underflow before mismatch amplifies it. Combine
            // binary exponents only after normalizing; no wider type is needed.
            int frequency_exponent = 0;
            int delay_exponent = 0;
            int coupling_exponent = 0;
            const double frequency_fraction = std::frexp(frequency_hz, &frequency_exponent);
            const double delay_fraction = std::frexp(delay_s_, &delay_exponent);
            const double coupling_fraction = std::frexp(coupling_, &coupling_exponent);
            normalized_sine =
                std::ldexp(-two_pi * frequency_fraction * delay_fraction / coupling_fraction,
                           frequency_exponent + delay_exponent - coupling_exponent);
            sine = normalized_sine * coupling_;
        }
        const Complex normalized_defect{attenuation_defect +
                                            2. * round_trip_magnitude * normalized_sine * sine,
                                        -2. * round_trip_magnitude * normalized_sine * cosine};
        const Complex denominator = 1. + reflection_ * reflection_ * normalized_defect;
        // Complex division would subtract nearly equal terms in Im(S11) near
        // a quarter wave. Its exact numerator simplifies to reflection * Im(D).
        const double denominator_norm = std::norm(denominator);
        const Complex reflected{reflection_ *
                                    (normalized_defect.real() * denominator.real() +
                                     normalized_defect.imag() * denominator.imag()) /
                                    denominator_norm,
                                reflection_ * normalized_defect.imag() / denominator_norm};
        const double attenuation_denominator = 1. + reflection_ * reflection_ * attenuation_defect;
        const Complex transmitted{
            magnitude * (cosine * attenuation_denominator / denominator_norm),
            magnitude * ((sine * attenuation_denominator +
                          2. * reflection_ * reflection_ * round_trip_magnitude * normalized_sine) /
                         denominator_norm)};
        return {2, {reflected, transmitted, transmitted, reflected}};
    }
};
} // namespace rfmodel
