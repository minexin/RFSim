#pragma once
#include "frequency_conversion.hpp"
#include <map>

namespace rfmodel {
struct PhaseNoiseOffset {
    int offset_bin{};
    double ssb_dbc_per_hz{};
};

// First-order x(t) = carrier * (1 + j*phi(t)), real stationary phi.
// Positive offset coefficients u are proper, E[|u|^2] = 10^(L/10).
// Sidebands are j*carrier*u and j*carrier*conj(u), hence P+- is negative.
inline ConversionNoise phase_noise_sidebands(const std::vector<ConversionChannel> &channels,
                                             std::size_t carrier_channel,
                                             Complex carrier_wave,
                                             const std::vector<PhaseNoiseOffset> &offsets) {
    const auto n = channels.size();
    const double scale = std::max(std::abs(carrier_wave.real()), std::abs(carrier_wave.imag()));
    if (!n || n > 512 || carrier_channel >= n || offsets.empty() || offsets.size() > 255 ||
        !conversion_detail::finite(carrier_wave) || scale <= 0.) {
        throw std::invalid_argument("invalid phase noise channels, carrier or offsets");
    }
    std::map<std::pair<std::size_t, int>, std::size_t> indices;
    for (std::size_t i = 0; i < n; ++i) {
        if (channels[i].port >= 1024 || channels[i].bin < 0 ||
            !indices.emplace(std::make_pair(channels[i].port, channels[i].bin), i).second) {
            throw std::invalid_argument("invalid or duplicate phase noise channel");
        }
    }
    const auto carrier = channels[carrier_channel];
    if (carrier.bin <= 0) {
        throw std::invalid_argument("phase noise requires a positive-frequency carrier");
    }
    const Complex scaled_wave = carrier_wave / scale;
    const double scaled_amplitude = std::abs(scaled_wave);
    const Complex unit = scaled_wave / scaled_amplitude;
    const double log_amplitude = std::log(scale) + std::log(scaled_amplitude);
    const Complex pair_phase = -unit * unit;
    ConversionNoise result{conversion_detail::zero(n), conversion_detail::zero(n)};
    int previous = 0;
    for (const auto &offset : offsets) {
        if (offset.offset_bin <= previous || offset.offset_bin >= carrier.bin ||
            offset.offset_bin > std::numeric_limits<int>::max() - carrier.bin ||
            !std::isfinite(offset.ssb_dbc_per_hz)) {
            throw std::invalid_argument("phase noise offsets must increase and stay above DC");
        }
        previous = offset.offset_bin;
        const auto lower = indices.find({carrier.port, carrier.bin - offset.offset_bin});
        const auto upper = indices.find({carrier.port, carrier.bin + offset.offset_bin});
        if (lower == indices.end() || upper == indices.end()) {
            throw std::invalid_argument("both phase noise sidebands must be declared");
        }
        // Compute the absolute PSD without overflowing the intermediate carrier power.
        const double log_density =
            2. * log_amplitude + offset.ssb_dbc_per_hz * (std::log(10.) / 10.);
        const double density = std::exp(log_density);
        if (!std::isfinite(density) || density < std::numeric_limits<double>::min()) {
            throw std::overflow_error("phase noise density must be finite and normal");
        }
        const auto l = lower->second, u = upper->second;
        result.covariance(l, l) = density;
        result.covariance(u, u) = density;
        result.complementary(l, u) = pair_phase * density;
        result.complementary(u, l) = pair_phase * density;
    }
    return result;
}
} // namespace rfmodel
