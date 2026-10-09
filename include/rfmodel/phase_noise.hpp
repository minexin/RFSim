#pragma once
#include "frequency_conversion.hpp"
#include <map>

namespace rfmodel {
struct PhaseNoiseOffset {
    int offset_bin{};
    double ssb_dbc_per_hz{};
};

struct PhaseNoiseCarrier {
    std::size_t channel{};
    Complex wave{};
    double phase_gain{1.};
};

// Every carrier sees phi_member = phase_gain * phi_reference. For each positive
// offset, z = U*u + V*conj(u), E[|u|^2]=1 and E[u^2]=0. Sum amplitudes first when
// sidebands coincide; C=U*U^H+V*V^H and P=U*V^T+V*U^T retain all correlations.
inline ConversionNoise phase_noise_group(const std::vector<ConversionChannel> &channels,
                                         const std::vector<PhaseNoiseCarrier> &carriers,
                                         const std::vector<PhaseNoiseOffset> &offsets) {
    const auto n = channels.size();
    if (!n || n > 512 || carriers.empty() || carriers.size() > n || offsets.empty() ||
        offsets.size() > 255) {
        throw std::invalid_argument("invalid phase noise channels, carriers or offsets");
    }
    std::map<std::pair<std::size_t, int>, std::size_t> indices;
    for (std::size_t i = 0; i < n; ++i) {
        if (channels[i].port >= 1024 || channels[i].bin < 0 ||
            !indices.emplace(std::make_pair(channels[i].port, channels[i].bin), i).second) {
            throw std::invalid_argument("invalid or duplicate phase noise channel");
        }
    }
    std::set<std::size_t> seen;
    std::vector<Complex> phase;
    std::vector<double> log_amplitude;
    for (const auto &carrier : carriers) {
        const double scale = std::max(std::abs(carrier.wave.real()), std::abs(carrier.wave.imag()));
        if (carrier.channel >= n || !seen.insert(carrier.channel).second ||
            channels[carrier.channel].bin <= 0 || !conversion_detail::finite(carrier.wave) ||
            scale <= 0. || !std::isfinite(carrier.phase_gain)) {
            throw std::invalid_argument("invalid phase noise carrier or phase gain");
        }
        const Complex scaled = carrier.wave / scale;
        const double amplitude = std::abs(scaled);
        phase.push_back(Complex{0., std::copysign(1., carrier.phase_gain)} * (scaled / amplitude));
        log_amplitude.push_back(std::log(scale) + std::log(amplitude));
    }
    ConversionNoise result{conversion_detail::zero(n), conversion_detail::zero(n)};
    int previous = 0;
    for (const auto &offset : offsets) {
        if (offset.offset_bin <= previous || !std::isfinite(offset.ssb_dbc_per_hz)) {
            throw std::invalid_argument("phase noise offsets must strictly increase");
        }
        previous = offset.offset_bin;
        std::vector<Complex> upper(n), lower(n);
        for (std::size_t k = 0; k < carriers.size(); ++k) {
            const auto &member = carriers[k];
            const auto channel = channels[member.channel];
            if (offset.offset_bin >= channel.bin ||
                offset.offset_bin > std::numeric_limits<int>::max() - channel.bin) {
                throw std::invalid_argument(
                    "phase noise sidebands must be representable and above DC");
            }
            const auto l = indices.find({channel.port, channel.bin - offset.offset_bin});
            const auto u = indices.find({channel.port, channel.bin + offset.offset_bin});
            if (l == indices.end() || u == indices.end()) {
                throw std::invalid_argument("both phase noise sidebands must be declared");
            }
            if (member.phase_gain == 0.) {
                continue;
            }
            const double log_density =
                2. * (log_amplitude[k] + std::log(std::abs(member.phase_gain))) +
                offset.ssb_dbc_per_hz * (std::log(10.) / 10.);
            const double density = std::exp(log_density);
            if (!std::isfinite(density) || density < std::numeric_limits<double>::min()) {
                throw std::overflow_error("phase noise density must be finite and normal");
            }
            const auto coefficient = phase[k] * std::sqrt(density);
            upper[u->second] += coefficient;
            lower[l->second] += coefficient;
        }
        std::vector<std::size_t> active;
        for (std::size_t i = 0; i < n; ++i) {
            if (upper[i] != Complex{} || lower[i] != Complex{}) {
                active.push_back(i);
            }
        }
        for (auto i : active) {
            for (auto j : active) {
                result.covariance(i, j) +=
                    upper[i] * std::conj(upper[j]) + lower[i] * std::conj(lower[j]);
                result.complementary(i, j) += upper[i] * lower[j] + lower[i] * upper[j];
            }
        }
    }
    noise_detail::finite_matrix(result.covariance);
    noise_detail::finite_matrix(result.complementary);
    return result;
}

inline ConversionNoise phase_noise_sidebands(const std::vector<ConversionChannel> &channels,
                                             std::size_t carrier_channel,
                                             Complex carrier_wave,
                                             const std::vector<PhaseNoiseOffset> &offsets) {
    return phase_noise_group(channels, {{carrier_channel, carrier_wave, 1.}}, offsets);
}
} // namespace rfmodel
