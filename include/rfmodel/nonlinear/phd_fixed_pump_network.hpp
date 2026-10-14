#pragma once
#include "rfmodel/nonlinear/phd_coefficient_slice.hpp"
#include <cmath>
#include <complex>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rfmodel {

// Values are in the named device channel's existing RMS power-wave coordinates.
struct PhdChannelBoundary {
    ConversionChannel channel;
    Complex source{}, reflection{};
};

struct PhdPerturbationLimit {
    ConversionChannel channel;
    double maximum_magnitude_sqrt_w{};
};

struct PhdChannelOffset {
    ConversionChannel channel;
    Complex outgoing{};
};

struct PhdPortPlane {
    std::size_t port{};
    std::string reference_plane;
};

// A prescribed deterministic affine map, not a nonlinear callback. All channel
// records must be complete, unique, and explicitly labelled. No noise input.
struct PhdAffineEnvironment {
    FrequencyConversionModel model;
    std::vector<PhdChannelBoundary> boundaries;
    std::vector<PhdChannelOffset> output_offsets;
    std::vector<PhdPortPlane> reference_planes;
};

struct PhdNetworkChannelState {
    std::size_t device{}; // The PHD slice is device 0; environments start at 1.
    ConversionChannel channel;
    Complex reference_ohms;
    std::string reference_plane;
    Complex incident, outgoing;
};

struct PhdFixedPumpResult {
    std::vector<PhdNetworkChannelState> channels;
    std::vector<Complex> phd_perturbations; // Frozen slice's original channel order.
    double relative_residual{};
    Complex prescribed_pump, solved_pump;
    bool pump_clamp_exact{};
};

// Owns the full restricted problem. There is deliberately no unrestricted model,
// ConversionDevice, noise analysis, or operating-point/Newton factory or getter.
class PhdFixedPumpNetwork {
    using Key = std::pair<std::size_t, int>;

    struct Block {
        FrequencyConversionModel model;
        std::vector<Complex> source, reflection, offset;
        std::map<std::size_t, std::string> planes;
    };

    PhdFrozenSlice frozen_;
    std::vector<Block> blocks_;
    std::vector<double> limits_;
    std::vector<std::size_t> offsets_;
    std::vector<std::pair<std::size_t, std::size_t>> connections_;
    std::set<std::size_t> connected_;
    std::size_t total_{};

    static Key key(ConversionChannel channel) {
        return {channel.port, channel.bin};
    }

    template <class Record>
    static std::vector<Record> align(const std::vector<ConversionChannel> &channels,
                                     const std::vector<Record> &records) {
        if (channels.size() != records.size()) {
            throw std::invalid_argument("PHD network requires complete labelled channel records");
        }
        std::map<Key, Record> indexed;
        for (const auto &record : records) {
            if (!indexed.emplace(key(record.channel), record).second) {
                throw std::invalid_argument("PHD network duplicate channel record");
            }
        }
        std::vector<Record> result;
        for (auto channel : channels) {
            const auto found = indexed.find(key(channel));
            if (found == indexed.end()) {
                throw std::invalid_argument("PHD network unknown or missing channel label");
            }
            result.push_back(found->second);
        }
        return result;
    }

    static void validate_wave(Complex value, int bin) {
        if (!conversion_detail::finite(value) || (bin == 0 && value.imag() != 0.)) {
            throw std::invalid_argument("PHD network requires finite waves and real DC");
        }
    }

    static Block block(FrequencyConversionModel model,
                       const std::vector<PhdChannelBoundary> &boundaries,
                       const std::vector<PhdChannelOffset> &output_offsets,
                       const std::vector<PhdPortPlane> &reference_planes) {
        const auto aligned = align(model.channels(), boundaries);
        const auto shifted = align(model.channels(), output_offsets);
        std::map<std::size_t, std::string> planes;
        for (const auto &plane : reference_planes) {
            if (plane.reference_plane.empty() ||
                !planes.emplace(plane.port, plane.reference_plane).second) {
                throw std::invalid_argument("PHD network invalid or duplicate reference plane");
            }
        }
        std::set<std::size_t> ports;
        std::vector<Complex> source, reflection, offset;
        for (std::size_t i = 0; i < aligned.size(); ++i) {
            const auto channel = model.channels()[i];
            if (!planes.count(channel.port)) {
                throw std::invalid_argument("PHD network missing reference plane");
            }
            ports.insert(channel.port);
            validate_wave(aligned[i].source, channel.bin);
            validate_wave(aligned[i].reflection, channel.bin);
            validate_wave(shifted[i].outgoing, channel.bin);
            source.push_back(aligned[i].source);
            reflection.push_back(aligned[i].reflection);
            offset.push_back(shifted[i].outgoing);
        }
        if (ports.size() != planes.size()) {
            throw std::invalid_argument("PHD network extra reference plane");
        }
        return {std::move(model),
                std::move(source),
                std::move(reflection),
                std::move(offset),
                std::move(planes)};
    }

    std::map<int, std::size_t> port_channels(std::size_t device, std::size_t port) const {
        const auto &channels = blocks_.at(device).model.channels();
        std::map<int, std::size_t> result;
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].port == port) {
                result.emplace(channels[i].bin, offsets_[device] + i);
            }
        }
        if (result.empty()) {
            throw std::invalid_argument("PHD network unknown physical port");
        }
        return result;
    }

public:
    PhdFixedPumpNetwork(PhdFrozenSlice frozen,
                        const std::vector<PhdChannelBoundary> &boundaries,
                        const std::vector<PhdPerturbationLimit> &limits)
        : frozen_(std::move(frozen)) {
        const auto &data = frozen_.coefficients();
        const auto n = data.channels.size();
        const auto labelled_boundaries = align(data.channels, boundaries);
        // Reject any pump freedom before constructing even the private map.
        const auto &pump_boundary = labelled_boundaries[data.pump_channel];
        if (pump_boundary.source != frozen_.nominal_incident()[data.pump_channel] ||
            pump_boundary.reflection != Complex{}) {
            throw std::invalid_argument(
                "PHD pump requires exact nominal source and zero reflection");
        }
        auto direct = conversion_detail::zero(n), conjugate = direct;
        std::vector<Complex> references;
        std::vector<PhdChannelOffset> output_offsets;
        std::vector<PhdPortPlane> planes;
        std::vector<ConversionChannel> nonpump;
        for (std::size_t i = 0; i < n; ++i) {
            references.emplace_back(data.port_reference_ohms[data.channels[i].port]);
            output_offsets.push_back({data.channels[i], frozen_.output_offset()[i]});
            if (i != data.pump_channel) {
                nonpump.push_back(data.channels[i]);
            }
            for (std::size_t j = 0; j < n; ++j) {
                direct(i, j) = frozen_.perturbation_jacobian().direct(i, j);
                conjugate(i, j) = frozen_.perturbation_jacobian().conjugate(i, j);
            }
        }
        for (std::size_t port = 0; port < data.port_reference_planes.size(); ++port) {
            planes.push_back({port, data.port_reference_planes[port]});
        }
        const auto bounded = align(nonpump, limits);
        limits_.resize(n);
        std::size_t next = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (i == data.pump_channel) {
                continue;
            }
            const double maximum = bounded[next++].maximum_magnitude_sqrt_w;
            if (!std::isfinite(maximum) || maximum < 0.) {
                throw std::invalid_argument(
                    "PHD perturbation limits must be finite and nonnegative");
            }
            limits_[i] = maximum;
        }
        auto owned = block(FrequencyConversionModel(data.fundamental_hz,
                                                    data.channels,
                                                    std::move(direct),
                                                    std::move(conjugate),
                                                    std::move(references)),
                           boundaries,
                           output_offsets,
                           planes);
        blocks_.push_back(std::move(owned));
        offsets_.push_back(0);
        total_ = n;
    }

    std::size_t add_environment(PhdAffineEnvironment environment) {
        const auto n = environment.model.channels().size();
        if (environment.model.spacing_hz() != frozen_.coefficients().fundamental_hz ||
            n > 512 - total_) {
            throw std::invalid_argument("PHD environment frequency step or channel count differs");
        }
        auto owned = block(std::move(environment.model),
                           environment.boundaries,
                           environment.output_offsets,
                           environment.reference_planes);
        // Allocate both containers before committing the new block.
        blocks_.reserve(blocks_.size() + 1);
        offsets_.reserve(offsets_.size() + 1);
        blocks_.push_back(std::move(owned));
        offsets_.push_back(total_);
        total_ += n;
        return blocks_.size() - 1;
    }

    // Ideal physical wire joins ALL supplied bins on these two planes. Names
    // identify coordinates, need not match, and imply no de-embedding or delay.
    // Different reference impedances use the existing power-wave V/I junction.
    void connect(std::size_t first_device,
                 std::size_t first_port,
                 std::size_t second_device,
                 std::size_t second_port) {
        const auto pump_port =
            frozen_.coefficients().channels[frozen_.coefficients().pump_channel].port;
        if ((first_device == 0 && first_port == pump_port) ||
            (second_device == 0 && second_port == pump_port)) {
            throw std::invalid_argument("PHD pump physical port must remain external");
        }
        if (first_device == second_device && first_port == second_port) {
            throw std::invalid_argument("PHD network cannot connect a port to itself");
        }
        const auto first = port_channels(first_device, first_port);
        const auto second = port_channels(second_device, second_port);
        if (first.size() != second.size()) {
            throw std::invalid_argument("PHD connected physical ports have different bins");
        }
        std::vector<std::pair<std::size_t, std::size_t>> pending;
        for (const auto &entry : first) {
            const auto found = second.find(entry.first);
            if (found == second.end() || connected_.count(entry.second) ||
                connected_.count(found->second)) {
                throw std::invalid_argument("PHD connected bin mismatch or already wired port");
            }
            const auto i = entry.second - offsets_[first_device];
            const auto j = found->second - offsets_[second_device];
            if (blocks_[first_device].source[i] != Complex{} ||
                blocks_[first_device].reflection[i] != Complex{} ||
                blocks_[second_device].source[j] != Complex{} ||
                blocks_[second_device].reflection[j] != Complex{}) {
                throw std::invalid_argument(
                    "PHD connected channels cannot have source or load boundaries");
            }
            pending.emplace_back(entry.second, found->second);
        }
        auto next_connected = connected_;
        auto next_connections = connections_;
        for (const auto &pair : pending) {
            next_connected.insert(pair.first);
            next_connected.insert(pair.second);
            next_connections.push_back(pair);
        }
        connected_.swap(next_connected);
        connections_.swap(next_connections);
    }

    PhdFixedPumpResult solve() const {
        auto direct = conversion_detail::zero(total_), conjugate = direct;
        std::vector<Complex> source, reflection, offset, references;
        std::vector<ConversionChannel> channels;
        std::size_t next_port = 0;
        for (std::size_t d = 0; d < blocks_.size(); ++d) {
            const auto &b = blocks_[d];
            const auto &local = b.model.channels();
            std::map<std::size_t, std::size_t> ports;
            for (std::size_t i = 0; i < local.size(); ++i) {
                if (!ports.count(local[i].port)) {
                    ports.emplace(local[i].port, next_port++);
                }
                channels.push_back({ports.at(local[i].port), local[i].bin});
                source.push_back(b.source[i]);
                reflection.push_back(b.reflection[i]);
                offset.push_back(b.offset[i]);
                references.push_back(b.model.reference_impedances_ohms()[i]);
                for (std::size_t j = 0; j < local.size(); ++j) {
                    direct(offsets_[d] + i, offsets_[d] + j) = b.model.direct()(i, j);
                    conjugate(offsets_[d] + i, offsets_[d] + j) = b.model.conjugate()(i, j);
                }
            }
        }
        // Only this private assembly sees the unrestricted affine representation.
        // The pump is an unconnected, reflection-free prescribed source, not a
        // Newton variable or a post-solve approximation to a clamp.
        const FrequencyConversionModel assembled(frozen_.coefficients().fundamental_hz,
                                                 std::move(channels),
                                                 std::move(direct),
                                                 std::move(conjugate),
                                                 std::move(references));
        const auto waves = assembled.solve_waves(source, reflection, connections_, offset);
        PhdFixedPumpResult result;
        const auto pump = frozen_.coefficients().pump_channel;
        result.prescribed_pump = frozen_.nominal_incident()[pump];
        result.solved_pump = waves.incident[pump];
        result.pump_clamp_exact = result.solved_pump == result.prescribed_pump;
        if (!result.pump_clamp_exact) {
            throw std::runtime_error("PHD fixed pump exact invariant failed");
        }
        const auto &nominal = frozen_.nominal_incident();
        for (std::size_t i = 0; i < nominal.size(); ++i) {
            const auto delta = waves.incident[i] - nominal[i];
            const auto magnitude = std::abs(delta);
            if (!conversion_detail::finite(delta) || !std::isfinite(magnitude) ||
                magnitude > limits_[i]) {
                throw std::domain_error(
                    "PHD solved perturbation exceeds caller-supplied domain at channel " +
                    std::to_string(i));
            }
            result.phd_perturbations.push_back(delta);
        }
        for (std::size_t d = 0; d < blocks_.size(); ++d) {
            const auto &b = blocks_[d];
            for (std::size_t i = 0; i < b.model.channels().size(); ++i) {
                const auto channel = b.model.channels()[i];
                result.channels.push_back({d,
                                           channel,
                                           b.model.reference_impedances_ohms()[i],
                                           b.planes.at(channel.port),
                                           waves.incident[offsets_[d] + i],
                                           waves.outgoing[offsets_[d] + i]});
            }
        }
        result.relative_residual = waves.relative_residual;
        return result;
    }
};
} // namespace rfmodel
