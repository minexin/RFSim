#pragma once
#include "frequency_conversion.hpp"
#include <map>

namespace rfmodel {
struct ConversionNoiseAnalysis {
    double reference_gain{};
    double reference_output_noise_w_per_hz{};
    double output_noise_w_per_hz{};
    double noise_factor{};
    double noise_figure_db{};
    double equivalent_input_temperature_k{};
};

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

    // Additional source noise may correlate external channels across devices. It is
    // independent of the per-device source blocks and is validated before addition.
    ConversionResult analyze(bool loaded_noise = false,
                             const ConversionNoise *additional_source_noise = nullptr,
                             const std::vector<Complex> &output_offset = {}) const {
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
        if (additional_source_noise) {
            combined.validate_noise(source_noise);
            combined.validate_noise(*additional_source_noise);
            for (auto i : connected_) {
                for (std::size_t j = 0; j < total_; ++j) {
                    for (const ConversionNoise *noise :
                         {static_cast<const ConversionNoise *>(&source_noise),
                          additional_source_noise}) {
                        if (noise->covariance(i, j) != Complex{} ||
                            noise->covariance(j, i) != Complex{} ||
                            noise->complementary(i, j) != Complex{} ||
                            noise->complementary(j, i) != Complex{}) {
                            throw std::invalid_argument(
                                "connected channel cannot have boundary noise");
                        }
                    }
                }
            }
            for (std::size_t i = 0; i < total_ * total_; ++i) {
                source_noise.covariance.values[i] += additional_source_noise->covariance.values[i];
                source_noise.complementary.values[i] +=
                    additional_source_noise->complementary.values[i];
            }
        }
        return combined.analyze(source,
                                reflection,
                                source_noise,
                                intrinsic_noise,
                                connections_,
                                loaded_noise,
                                output_offset);
    }

    // A separate reference-temperature experiment. Original deterministic sources
    // and source noise are replaced; intrinsic device noise and all reflections remain.
    // Reference bands are independent, phase-averaged available-power inputs.
    ConversionNoiseAnalysis
    reference_noise_analysis(const std::vector<std::size_t> &reference_channels,
                             const std::vector<std::size_t> &thermal_channels,
                             std::size_t output_channel,
                             double temperature_k = 290.) const {
        if (!std::isfinite(temperature_k) || temperature_k <= 0. || reference_channels.empty() ||
            thermal_channels.empty() || reference_channels.size() > total_ ||
            thermal_channels.size() > total_) {
            throw std::invalid_argument("invalid conversion noise reference");
        }
        const double kt = 1.380649e-23 * temperature_k;
        if (!std::isfinite(kt) || kt == 0.) {
            throw std::overflow_error("conversion noise reference range");
        }
        auto position = [&](std::size_t index) {
            if (index >= total_ || connected_.count(index)) {
                throw std::invalid_argument("noise reference must be an external channel");
            }
            std::size_t device = 0;
            while (device + 1 < offsets_.size() && offsets_[device + 1] <= index) {
                ++device;
            }
            const auto local = index - offsets_[device];
            if (devices_[device].model.channels()[local].bin == 0) {
                throw std::invalid_argument("noise figure requires positive-frequency channels");
            }
            return std::make_pair(device, local);
        };
        const auto output = position(output_channel);
        std::set<std::size_t> thermal_set, reference_set;
        for (auto index : thermal_channels) {
            position(index);
            if (index == output_channel || !thermal_set.insert(index).second) {
                throw std::invalid_argument("repeated thermal channel or heated output load");
            }
        }
        for (auto index : reference_channels) {
            if (!thermal_set.count(index) || !reference_set.insert(index).second) {
                throw std::invalid_argument("reference channels must be a unique thermal subset");
            }
        }
        auto measured = *this;
        for (std::size_t d = 0; d < devices_.size(); ++d) {
            auto &device = measured.devices_[d];
            const auto n = device.model.channels().size();
            device.source.assign(n, Complex{});
            device.source_noise = device.model.zero_noise();
            for (std::size_t i = 0; i < n; ++i) {
                const auto gamma = device.reflection[i];
                if (!conversion_detail::finite(gamma) || std::norm(gamma) > 1.) {
                    throw std::invalid_argument("noise analysis requires passive terminations");
                }
            }
        }
        auto reference = measured;
        for (auto &device : reference.devices_) {
            device.intrinsic_noise = device.model.zero_noise();
        }
        for (auto index : thermal_channels) {
            const auto location = position(index);
            auto &device = measured.devices_[location.first];
            const double accepted = 1. - std::norm(device.reflection[location.second]);
            const double variance = kt * accepted;
            if (variance == 0.) {
                throw std::overflow_error("conversion thermal noise underflow");
            }
            device.source_noise.covariance(location.second, location.second) = variance;
            if (reference_set.count(index)) {
                reference.devices_[location.first].source_noise.covariance(
                    location.second, location.second) = accepted;
            }
        }
        const auto full = measured.analyze();
        const auto probe = reference.analyze();
        const double load = 1. - std::norm(devices_[output.first].reflection[output.second]);
        ConversionNoiseAnalysis result;
        result.reference_gain =
            load * probe.outgoing_noise.covariance(output_channel, output_channel).real();
        result.output_noise_w_per_hz =
            load * full.outgoing_noise.covariance(output_channel, output_channel).real();
        result.reference_output_noise_w_per_hz = kt * result.reference_gain;
        if (!std::isfinite(result.reference_gain) || result.reference_gain <= 0. ||
            !std::isfinite(result.output_noise_w_per_hz) ||
            !std::isfinite(result.reference_output_noise_w_per_hz) ||
            result.reference_output_noise_w_per_hz <= 0.) {
            throw std::domain_error("zero or unrepresentable conversion reference gain/noise");
        }
        result.noise_factor = result.output_noise_w_per_hz / result.reference_output_noise_w_per_hz;
        if (!std::isfinite(result.noise_factor) ||
            result.noise_factor < 1. - 1024. * total_ * std::numeric_limits<double>::epsilon()) {
            throw std::domain_error("invalid conversion noise factor");
        }
        result.noise_factor = std::max(1., result.noise_factor);
        result.noise_figure_db = 10. * std::log10(result.noise_factor);
        result.equivalent_input_temperature_k = temperature_k * (result.noise_factor - 1.);
        if (!std::isfinite(result.equivalent_input_temperature_k)) {
            throw std::overflow_error("conversion equivalent noise temperature overflow");
        }
        return result;
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
