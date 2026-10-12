#pragma once
#include "am_pm.hpp"
#include "conversion_operating_point.hpp"

namespace rfmodel {
// Rotate each explicitly selected RF outgoing wave using its own power.
// Composition is after the base deterministic response, before fixed network
// offsets. Selection does not imply vendor multitone/harmonic phase semantics.
inline ConversionLinearization apply_output_am_pm(const ConversionLinearization &base,
                                                  const LegacyPointDerivativeAmPm &phase,
                                                  const std::vector<std::size_t> &output_channels) {
    const auto &channels = base.jacobian.channels();
    const auto n = channels.size();
    if (!n || n > 512 || base.outgoing.size() != n) {
        throw std::invalid_argument("invalid AM-PM linearization dimensions");
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!conversion_detail::finite(base.outgoing[i]) ||
            (channels[i].bin == 0 && base.outgoing[i].imag() != 0.)) {
            throw std::invalid_argument("invalid AM-PM base output");
        }
    }
    std::set<std::size_t> selected;
    for (auto i : output_channels) {
        if (i >= n || channels[i].bin == 0 || !selected.insert(i).second) {
            throw std::invalid_argument("invalid or duplicate AM-PM RF output channel");
        }
    }
    auto direct = base.jacobian.direct(), conjugate = base.jacobian.conjugate();
    auto outgoing = base.outgoing;
    for (auto row : selected) {
        const auto wave = base.outgoing[row];
        const double power = std::norm(wave);
        const auto response = phase.response(power);
        const auto rotation = std::polar(1., response.phase_rad);
        const Complex radial(0., .5 * response.radial_phase_slope);
        const auto local_direct = rotation * (1. + radial);
        Complex local_conjugate{};
        if (power > 0.) {
            const auto direction = wave / std::sqrt(power);
            local_conjugate = rotation * radial * direction * direction;
        }
        for (std::size_t column = 0; column < n; ++column) {
            const auto a = base.jacobian.direct()(row, column);
            const auto b = base.jacobian.conjugate()(row, column);
            direct(row, column) = local_direct * a + local_conjugate * std::conj(b);
            conjugate(row, column) = local_direct * b + local_conjugate * std::conj(a);
        }
        outgoing[row] = rotation * wave;
        if (!conversion_detail::finite(outgoing[row])) {
            throw std::overflow_error("AM-PM rotated wave overflow");
        }
    }
    return {FrequencyConversionModel(base.jacobian.spacing_hz(),
                                     channels,
                                     direct,
                                     conjugate,
                                     base.jacobian.reference_ohms()),
            outgoing};
}
} // namespace rfmodel
