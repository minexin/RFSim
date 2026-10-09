#pragma once
#include "frequency_conversion.hpp"
#include <map>

namespace rfmodel {
struct ConversionDevice {
    FrequencyConversionModel model;
    std::vector<Complex> source, reflection;
    ConversionNoise source_noise, intrinsic_noise;
};

// Physical wires join all bins of two ports. Independent device noise blocks
// retain full within-device C/P, including correlations between frequencies.
class FrequencyConversionNetwork {
    double spacing_, reference_;
    std::vector<ConversionDevice> devices_;
    std::vector<std::size_t> offsets_;
    std::vector<std::pair<std::size_t, std::size_t>> connections_;
    std::set<std::size_t> connected_;
    std::size_t total_{};

    std::map<int, std::size_t> port_channels(std::size_t device, std::size_t port) const {
        if (device >= devices_.size()) {
            throw std::out_of_range("conversion device index");
        }
        std::map<int, std::size_t> result;
        const auto &channels = devices_[device].model.channels();
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].port == port) {
                result.emplace(channels[i].bin, offsets_[device] + i);
            }
        }
        if (result.empty()) {
            throw std::invalid_argument("unknown conversion physical port");
        }
        return result;
    }

public:
    FrequencyConversionNetwork(double spacing_hz, double reference_ohms = 50.)
        : spacing_(spacing_hz), reference_(reference_ohms) {
        if (!std::isfinite(spacing_) || spacing_ <= 0. || !std::isfinite(reference_) ||
            reference_ <= 0.) {
            throw std::invalid_argument("invalid conversion network spacing/reference");
        }
    }

    std::size_t channel_count() const {
        return total_;
    }

    std::size_t channel_offset(std::size_t device) const {
        return offsets_.at(device);
    }

    std::size_t add(ConversionDevice device) {
        const auto n = device.model.channels().size();
        if (device.model.spacing_hz() != spacing_ || device.model.reference_ohms() != reference_ ||
            n > 512 - total_ || device.source.size() != n || device.reflection.size() != n) {
            throw std::invalid_argument("conversion device grid/reference/size differs");
        }
        for (const auto *matrix : {&device.source_noise.covariance,
                                   &device.source_noise.complementary,
                                   &device.intrinsic_noise.covariance,
                                   &device.intrinsic_noise.complementary}) {
            noise_detail::finite_matrix(*matrix);
            if (matrix->ports != n) {
                throw std::invalid_argument("conversion device noise dimensions differ");
            }
        }
        offsets_.push_back(total_);
        devices_.push_back(std::move(device));
        total_ += n;
        return devices_.size() - 1;
    }

    std::size_t add(FrequencyConversionModel model) {
        const auto n = model.channels().size();
        const auto zero = model.zero_noise();
        return add(
            {std::move(model), std::vector<Complex>(n), std::vector<Complex>(n), zero, zero});
    }

    void connect(std::size_t first_device,
                 std::size_t first_port,
                 std::size_t second_device,
                 std::size_t second_port) {
        if (first_device == second_device && first_port == second_port) {
            throw std::invalid_argument("cannot connect a conversion port to itself");
        }
        const auto first = port_channels(first_device, first_port);
        const auto second = port_channels(second_device, second_port);
        if (first.size() != second.size()) {
            throw std::invalid_argument("connected ports have different frequency grids");
        }
        std::vector<std::pair<std::size_t, std::size_t>> pending;
        for (const auto &entry : first) {
            const auto found = second.find(entry.first);
            if (found == second.end() || connected_.count(entry.second) ||
                connected_.count(found->second)) {
                throw std::invalid_argument(
                    "conversion frequency mismatch or port already connected");
            }
            pending.emplace_back(entry.second, found->second);
        }
        for (const auto &pair : pending) {
            connected_.insert(pair.first);
            connected_.insert(pair.second);
            connections_.push_back(pair);
        }
    }

    ConversionResult analyze() const {
        if (devices_.empty()) {
            throw std::invalid_argument("empty conversion network");
        }
        auto a = conversion_detail::zero(total_), b = a;
        ConversionNoise source_noise{a, a}, intrinsic_noise{a, a};
        std::vector<Complex> source(total_), reflection(total_);
        std::vector<ConversionChannel> channels;
        std::size_t next_port = 0;
        for (std::size_t d = 0; d < devices_.size(); ++d) {
            const auto &device = devices_[d];
            const auto offset = offsets_[d];
            const auto &local = device.model.channels();
            std::map<std::size_t, std::size_t> global_ports;
            for (std::size_t i = 0; i < local.size(); ++i) {
                if (!global_ports.count(local[i].port)) {
                    global_ports.emplace(local[i].port, next_port++);
                }
                channels.push_back({global_ports.at(local[i].port), local[i].bin});
                source[offset + i] = device.source[i];
                reflection[offset + i] = device.reflection[i];
                for (std::size_t j = 0; j < local.size(); ++j) {
                    a(offset + i, offset + j) = device.model.direct()(i, j);
                    b(offset + i, offset + j) = device.model.conjugate()(i, j);
                    source_noise.covariance(offset + i, offset + j) =
                        device.source_noise.covariance(i, j);
                    source_noise.complementary(offset + i, offset + j) =
                        device.source_noise.complementary(i, j);
                    intrinsic_noise.covariance(offset + i, offset + j) =
                        device.intrinsic_noise.covariance(i, j);
                    intrinsic_noise.complementary(offset + i, offset + j) =
                        device.intrinsic_noise.complementary(i, j);
                }
            }
        }
        const FrequencyConversionModel combined(spacing_, channels, a, b, reference_);
        return combined.analyze(source, reflection, source_noise, intrinsic_noise, connections_);
    }
};

// Stationary linear S/noise samples become independent frequency blocks. DC
// covariance is real and has P=C; positive-frequency stationary noise has P=0.
inline ConversionDevice lift_linear_conversion(double spacing_hz,
                                               const std::vector<int> &bins,
                                               const std::vector<SMatrix> &samples,
                                               const std::vector<NoiseCorrelation> &noise,
                                               double reference_ohms = 50.) {
    if (bins.empty() || bins.size() > 512 || samples.size() != bins.size() ||
        noise.size() != bins.size()) {
        throw std::invalid_argument("invalid lifted linear sample count");
    }
    const auto ports = samples[0].ports;
    if (!ports || ports > 512 / bins.size()) {
        throw std::invalid_argument("lifted linear channel limit");
    }
    const auto count = ports * bins.size();
    auto a = conversion_detail::zero(count), b = a;
    ConversionNoise intrinsic{a, a}, source_noise{a, a};
    std::vector<ConversionChannel> channels;
    for (std::size_t k = 0; k < bins.size(); ++k) {
        if (bins[k] < 0 || (k && bins[k] <= bins[k - 1])) {
            throw std::invalid_argument("linear bins must increase");
        }
        noise_detail::finite_matrix(samples[k]);
        noise_detail::finite_matrix(noise[k].watts_per_hz);
        if (samples[k].ports != ports || noise[k].watts_per_hz.ports != ports) {
            throw std::invalid_argument("lifted linear dimensions differ");
        }
        for (std::size_t i = 0; i < ports; ++i) {
            channels.push_back({i, bins[k]});
            for (std::size_t j = 0; j < ports; ++j) {
                a(k * ports + i, k * ports + j) = samples[k](i, j);
                intrinsic.covariance(k * ports + i, k * ports + j) = noise[k].watts_per_hz(i, j);
                if (bins[k] == 0) {
                    intrinsic.complementary(k * ports + i, k * ports + j) =
                        noise[k].watts_per_hz(i, j);
                }
            }
        }
    }
    FrequencyConversionModel model(spacing_hz, channels, a, b, reference_ohms);
    return {std::move(model),
            std::vector<Complex>(count),
            std::vector<Complex>(count),
            source_noise,
            intrinsic};
}
} // namespace rfmodel
