#pragma once
#include "frequency_conversion.hpp"
#include <map>

namespace rfmodel {
struct MixerLinearization {
    FrequencyConversionModel incremental_model;
    std::vector<Complex> operating_outgoing;

    // Bilinear Euler identity: J(a0)*a0 = 2*F(a0), hence d = -F(a0).
    std::vector<Complex> output_offset() const {
        auto result = operating_outgoing;
        for (auto &value : result) {
            value = -value;
        }
        return result;
    }
};

// RMS power-wave product y(t)=sqrt(2)*k*x_RF(t)*x_LO(t). The constant k is
// fixed at the supplied LO operating amplitude, never renormalized by noise.
inline MixerLinearization linearize_real_mixer(double spacing_hz,
                                               const std::vector<ConversionChannel> &channels,
                                               const std::vector<Complex> &operating_incident,
                                               int lo_bin,
                                               double gain_db,
                                               std::size_t rf_port = 0,
                                               std::size_t lo_port = 1,
                                               std::size_t if_port = 2,
                                               double reference_ohms = 50.) {
    const auto n = channels.size();
    if (!n || n > 512 || operating_incident.size() != n || !std::isfinite(spacing_hz) ||
        spacing_hz <= 0. || !std::isfinite(reference_ohms) || reference_ohms <= 0. || lo_bin <= 0 ||
        !std::isfinite(gain_db) || rf_port >= 1024 || lo_port >= 1024 || if_port >= 1024 ||
        rf_port == lo_port || rf_port == if_port || lo_port == if_port) {
        throw std::invalid_argument("invalid mixer linearization parameters");
    }
    std::map<std::pair<std::size_t, int>, std::size_t> indices;
    std::vector<std::size_t> rf, lo;
    Complex pump;
    for (std::size_t i = 0; i < n; ++i) {
        const auto channel = channels[i];
        if ((channel.port != rf_port && channel.port != lo_port && channel.port != if_port) ||
            channel.bin < 0 || !std::isfinite(channel.bin * spacing_hz) ||
            !indices.emplace(std::make_pair(channel.port, channel.bin), i).second ||
            !conversion_detail::finite(operating_incident[i]) ||
            (channel.bin == 0 && operating_incident[i].imag() != 0.)) {
            throw std::invalid_argument("invalid mixer channel or operating wave");
        }
        if (channel.port == rf_port) {
            rf.push_back(i);
        } else if (channel.port == lo_port) {
            lo.push_back(i);
            if (channel.bin == lo_bin) {
                pump = operating_incident[i];
            } else if (operating_incident[i] != Complex{}) {
                throw std::invalid_argument("LO operating point must be a single declared pump");
            }
        }
    }
    const double amplitude = std::abs(pump), gain = std::pow(10., gain_db / 20.);
    if (rf.empty() || !std::isfinite(amplitude) || amplitude <= 0. || !std::isfinite(gain) ||
        gain <= 0.) {
        throw std::invalid_argument("mixer needs RF channels and a finite nonzero LO pump");
    }
    const double k = gain / amplitude;
    if (!std::isfinite(k) || k <= 0.) {
        throw std::overflow_error("mixer normalization is not representable");
    }
    auto product = [&](const std::vector<Complex> &x, const std::vector<Complex> &l) {
        std::vector<Complex> out(n);
        auto add = [&](long long bin, Complex value) {
            if (bin < 0 || bin > std::numeric_limits<int>::max()) {
                throw std::overflow_error("mixer output frequency range");
            }
            const auto found = indices.find({if_port, static_cast<int>(bin)});
            if (found == indices.end()) {
                throw std::invalid_argument("missing nominal or incremental mixer output channel");
            }
            auto &target = out[found->second];
            target += value;
            if (!conversion_detail::finite(target)) {
                throw std::overflow_error("mixer product overflow");
            }
        };
        for (auto r : rf) {
            if (x[r] == Complex{}) {
                continue;
            }
            for (auto q : lo) {
                if (l[q] == Complex{}) {
                    continue;
                }
                const auto scaled_lo = k * l[q];
                if (!conversion_detail::finite(scaled_lo) || scaled_lo == Complex{}) {
                    throw std::overflow_error("mixer scaled LO is not representable");
                }
                const auto rb = channels[r].bin, lb = channels[q].bin;
                if (rb == 0 || lb == 0) {
                    add(static_cast<long long>(rb) + lb, std::sqrt(2.) * x[r] * scaled_lo);
                } else {
                    add(static_cast<long long>(rb) + lb, x[r] * scaled_lo);
                    if (rb > lb) {
                        add(rb - lb, x[r] * std::conj(scaled_lo));
                    } else if (rb < lb) {
                        add(lb - rb, std::conj(x[r]) * scaled_lo);
                    } else {
                        add(0, std::sqrt(2.) * (x[r] * std::conj(scaled_lo)).real());
                    }
                }
            }
        }
        return out;
    };
    const auto nominal = product(operating_incident, operating_incident);
    auto a = conversion_detail::zero(n), b = a;
    for (std::size_t column = 0; column < n; ++column) {
        const auto port = channels[column].port;
        if (port == if_port) {
            continue;
        }
        auto response = [&](Complex value) {
            std::vector<Complex> basis(n);
            basis[column] = value;
            return port == rf_port ? product(basis, operating_incident)
                                   : product(operating_incident, basis);
        };
        const auto real = response(1.);
        if (channels[column].bin == 0) {
            for (std::size_t row = 0; row < n; ++row) {
                a(row, column) = real[row];
            }
        } else {
            const auto imaginary = response(Complex{0., 1.});
            for (std::size_t row = 0; row < n; ++row) {
                a(row, column) = .5 * real[row] - Complex{0., .5} * imaginary[row];
                b(row, column) = .5 * real[row] + Complex{0., .5} * imaginary[row];
            }
        }
    }
    return {FrequencyConversionModel(spacing_hz, channels, a, b, reference_ohms), nominal};
}
} // namespace rfmodel
