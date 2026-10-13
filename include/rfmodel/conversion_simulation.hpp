#pragma once
#include "channel_noise.hpp"
#include "conversion_grid.hpp"
#include "conversion_power_wave.hpp"
#include "phase_noise.hpp"

namespace rfmodel {
struct ConversionPhaseMember {
    ConversionEndpoint carrier;
    double phase_gain{1.};
};

struct ConversionPhaseGroup {
    std::string name;
    std::vector<ConversionPhaseMember> members;
    std::vector<PhaseNoiseOffset> offsets;
};
enum class ConversionWaveDirection {
    incident,
    outgoing
};

struct ConversionChannelMeasurementRequest {
    std::string name;
    std::size_t device{}, port{};
    double center_hz{}, bandwidth_hz{};
    std::vector<int> desired_bins;
    ConversionWaveDirection direction{ConversionWaveDirection::outgoing};
};

struct ConversionNoiseExperiment {
    std::string name;
    std::vector<ConversionEndpoint> reference_channels, thermal_channels;
    ConversionEndpoint output;
    double temperature_k{290.};
};

struct ConversionSimulationRequest {
    std::vector<ConversionDevice> devices;
    std::vector<ConversionPortConnection> connections;
    std::vector<ConversionNonlinearDevice> nonlinear;
    std::optional<ConversionOperatingOptions> operating_point;
    std::vector<Complex> initial_incident, output_offset;
    std::optional<ConversionNoise> additional_source_noise;
    std::vector<ConversionPhaseGroup> phase_noise;
    double phase_reference_ohms{50.};
    bool loaded_noise{false};
    std::vector<ConversionChannelMeasurementRequest> measurements;
    std::vector<ConversionNoiseExperiment> noise_experiments;
};

struct ConversionChannelState {
    ConversionEndpoint channel;
    Complex reference_ohms{}, voltage_v{}, current_a{};
    double net_into_device_w{};
    bool connected{};
};

struct ConversionOperatingDiagnostics {
    std::size_t iterations{}, backtracks{};
    double scaled_residual{};
};

struct ConversionSimulationResult {
    ConversionResult waves;
    std::vector<ConversionChannelState> channels;
    std::vector<std::pair<std::string, ChannelNoiseMeasurement>> measurements;
    std::vector<std::pair<std::string, ConversionNoiseAnalysis>> noise_experiments;
    std::optional<ConversionOperatingDiagnostics> operating_point;
    // The accepted Jacobian/noise/offset snapshot, ready for further native
    // analysis. No post-convergence callback re-evaluation is performed.
    std::vector<ConversionDevice> accepted_devices;
    std::vector<Complex> output_offset;
    std::optional<ConversionNoise> additional_source_noise;
};

// Owns a request snapshot. All channel requests use physical identities; they
// survive grid reordering. Phase groups are independent of one another and of
// explicitly supplied source C/P; members in each group share one phase process.
inline ConversionSimulationResult
analyze_conversion_simulation(ConversionSimulationRequest request) {
    if (request.devices.empty() || request.devices.size() > 512 ||
        request.phase_noise.size() > 512 || request.measurements.size() > 512 ||
        request.noise_experiments.size() > 512 ||
        (!request.operating_point &&
         (!request.nonlinear.empty() || !request.initial_incident.empty()))) {
        throw std::invalid_argument("invalid conversion simulation request");
    }
    using Key = std::tuple<std::size_t, std::size_t, int>;
    const double spacing = request.devices.front().model.spacing_hz();
    std::map<Key, std::size_t> lookup;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> port_ids;
    std::set<std::pair<std::size_t, std::size_t>> connected;
    std::vector<ConversionEndpoint> labels;
    std::vector<ConversionChannel> global_channels;
    std::vector<Complex> source, reflection, references;
    auto make_network = [&](const std::vector<ConversionDevice> &devices) {
        FrequencyConversionNetwork network(spacing);
        for (const auto &device : devices) {
            network.add_power_waves(device);
        }
        for (const auto &wire : request.connections) {
            network.connect(
                wire.first_device, wire.first_port, wire.second_device, wire.second_port);
        }
        return network;
    };
    // Validate topology and local dimensions before resolving auxiliary requests.
    auto network = make_network(request.devices);
    for (const auto &wire : request.connections) {
        connected.emplace(wire.first_device, wire.first_port);
        connected.emplace(wire.second_device, wire.second_port);
    }
    for (std::size_t d = 0; d < request.devices.size(); ++d) {
        const auto &device = request.devices[d];
        const auto &channels = device.model.channels();
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const auto c = channels[i];
            lookup.emplace(Key{d, c.port, c.bin}, labels.size());
            labels.push_back({d, c.port, c.bin});
            const auto key = std::make_pair(d, c.port);
            if (!port_ids.count(key)) {
                port_ids.emplace(key, port_ids.size());
            }
            global_channels.push_back({port_ids.at(key), c.bin});
            references.push_back(device.model.reference_impedances_ohms()[i]);
            source.push_back(device.source[i]);
            reflection.push_back(device.reflection[i]);
        }
    }
    auto position = [&](const ConversionEndpoint &endpoint) {
        const auto found = lookup.find({endpoint.device, endpoint.port, endpoint.bin});
        if (found == lookup.end()) {
            throw std::invalid_argument("unknown conversion channel identity");
        }
        return found->second;
    };
    auto named = [](const std::string &name, std::set<std::string> &names) {
        if (name.empty() || name.size() > 1024 || name.find('\0') != std::string::npos ||
            !names.insert(name).second) {
            throw std::invalid_argument("invalid or duplicate conversion request name");
        }
    };
    const auto n = labels.size();
    if (!request.phase_noise.empty()) {
        if (!std::isfinite(request.phase_reference_ohms) || request.phase_reference_ohms <= 0.) {
            throw std::invalid_argument("invalid phase noise generation reference");
        }
        const auto zero = conversion_detail::zero(n);
        const FrequencyConversionModel original(spacing, global_channels, zero, zero, references);
        const std::vector<Complex> common_refs(n, request.phase_reference_ohms);
        const FrequencyConversionModel common(spacing, global_channels, zero, zero, common_refs);
        std::vector<std::size_t> external;
        for (std::size_t i = 0; i < n; ++i) {
            if (!connected.count({labels[i].device, labels[i].port})) {
                external.push_back(i);
            }
        }
        // Boundary-only coordinates avoid any pole in a device model transform.
        const auto boundary = renormalize_conversion_boundary(
            original, common_refs, external, source, reflection, original.zero_noise());
        auto generated = common.zero_noise();
        std::set<std::string> names;
        for (const auto &group : request.phase_noise) {
            named(group.name, names);
            std::vector<PhaseNoiseCarrier> carriers;
            for (const auto &member : group.members) {
                const auto i = position(member.carrier);
                if (connected.count({member.carrier.device, member.carrier.port})) {
                    throw std::invalid_argument(
                        "phase noise carrier must have an external boundary");
                }
                carriers.push_back({i, boundary.source[i], member.phase_gain});
            }
            const auto contribution = phase_noise_group(global_channels, carriers, group.offsets);
            for (std::size_t i = 0; i < n * n; ++i) {
                generated.covariance.values[i] += contribution.covariance.values[i];
                generated.complementary.values[i] += contribution.complementary.values[i];
            }
        }
        auto local =
            renormalize_conversion_boundary(
                common, references, external, boundary.source, boundary.reflection, generated)
                .source_noise;
        if (request.additional_source_noise) {
            original.validate_noise(*request.additional_source_noise);
            for (std::size_t i = 0; i < n * n; ++i) {
                local.covariance.values[i] += request.additional_source_noise->covariance.values[i];
                local.complementary.values[i] +=
                    request.additional_source_noise->complementary.values[i];
            }
        }
        original.validate_noise(local);
        request.additional_source_noise = std::move(local);
    }
    bool loaded = request.loaded_noise;
    std::set<std::string> measurement_names;
    for (const auto &measurement : request.measurements) {
        named(measurement.name, measurement_names);
        if (!port_ids.count({measurement.device, measurement.port}) ||
            (measurement.direction != ConversionWaveDirection::incident &&
             measurement.direction != ConversionWaveDirection::outgoing)) {
            throw std::invalid_argument("unknown conversion measurement port/direction");
        }
        std::set<int> desired;
        for (auto bin : measurement.desired_bins) {
            position({measurement.device, measurement.port, bin});
            if (!desired.insert(bin).second) {
                throw std::invalid_argument("duplicate desired spectral line");
            }
        }
        loaded = loaded || measurement.direction == ConversionWaveDirection::incident;
    }
    ConversionSimulationResult result;
    const auto *extra =
        request.additional_source_noise ? &*request.additional_source_noise : nullptr;
    if (request.operating_point) {
        auto solved = solve_conversion_operating_point_power_waves(request.devices,
                                                                   request.connections,
                                                                   request.nonlinear,
                                                                   request.initial_incident,
                                                                   *request.operating_point,
                                                                   loaded,
                                                                   extra,
                                                                   request.output_offset);
        result.waves = std::move(solved.waves);
        result.accepted_devices = std::move(solved.linearized_devices);
        result.output_offset = std::move(solved.output_offset);
        result.operating_point = ConversionOperatingDiagnostics{
            solved.iterations, solved.backtracks, solved.scaled_residual};
        network = make_network(result.accepted_devices);
    } else {
        result.waves = network.analyze(loaded, extra, request.output_offset);
        result.accepted_devices = std::move(request.devices);
        result.output_offset = request.output_offset.empty() ? std::vector<Complex>(n)
                                                             : std::move(request.output_offset);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto a = result.waves.incident[i], b = result.waves.outgoing[i], z = references[i];
        const double root = std::sqrt(z.real());
        const auto voltage = (std::conj(z) * a + z * b) / root;
        const auto current = (a - b) / root;
        const double power = std::norm(a) - std::norm(b);
        if (!conversion_detail::finite(voltage) || !conversion_detail::finite(current) ||
            !std::isfinite(power)) {
            throw std::overflow_error("conversion channel physical result overflow");
        }
        result.channels.push_back({labels[i],
                                   z,
                                   voltage,
                                   current,
                                   power,
                                   connected.count({labels[i].device, labels[i].port}) != 0});
    }
    for (const auto &measurement : request.measurements) {
        std::map<int, std::size_t> selected;
        for (std::size_t i = 0; i < n; ++i) {
            if (labels[i].device == measurement.device && labels[i].port == measurement.port) {
                selected.emplace(labels[i].bin, i);
            }
        }
        const bool incident = measurement.direction == ConversionWaveDirection::incident;
        const auto &noise = incident ? result.waves.incident_noise : result.waves.outgoing_noise;
        const auto &waves = incident ? result.waves.incident : result.waves.outgoing;
        const std::set<int> desired(measurement.desired_bins.begin(),
                                    measurement.desired_bins.end());
        std::vector<NoiseDensitySample> samples;
        std::vector<ChannelSignalLine> lines;
        for (auto entry : selected) {
            samples.push_back(
                {entry.first * spacing, noise.covariance(entry.second, entry.second).real()});
            if (desired.count(entry.first)) {
                lines.push_back({entry.first * spacing, std::norm(waves[entry.second])});
            }
        }
        result.measurements.emplace_back(
            measurement.name,
            measure_channel_noise(samples, lines, measurement.center_hz, measurement.bandwidth_hz));
    }
    std::set<std::string> experiment_names;
    for (const auto &experiment : request.noise_experiments) {
        named(experiment.name, experiment_names);
        std::vector<std::size_t> reference, thermal;
        for (const auto &channel : experiment.reference_channels) {
            reference.push_back(position(channel));
        }
        for (const auto &channel : experiment.thermal_channels) {
            thermal.push_back(position(channel));
        }
        result.noise_experiments.emplace_back(
            experiment.name,
            network.reference_noise_analysis(
                reference, thermal, position(experiment.output), experiment.temperature_k));
    }
    result.additional_source_noise = std::move(request.additional_source_noise);
    return result;
}
} // namespace rfmodel
