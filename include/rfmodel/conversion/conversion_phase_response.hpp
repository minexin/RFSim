#pragma once
#include "rfmodel/conversion/conversion_simulation.hpp"
#include "rfmodel/noise/phase_response.hpp"

namespace rfmodel {
struct ConversionPhaseResponseMember {
    ConversionEndpoint carrier;
};

struct ConversionPhaseResponseGroup {
    std::string name; // Unique diagnostic label only; never merges processes.
    std::vector<ConversionPhaseResponseMember> members;
    std::vector<PhaseResponsePoint> points;
};

// Return an owning request snapshot with generated full C/P added to existing
// additional_source_noise. Each group is statistically independent; only its
// members share a reference. Calling twice adds a second independent process:
// labels are not persistent identities and do not deduplicate or replace noise.
// No request layout or original real phase_gain semantics are changed.
// Generation uses one explicit positive real wave reference, then transforms
// each sideband's full C/P back to its own local (possibly complex) reference.
// Changing the generation reference defines a different source model in general.
inline ConversionSimulationRequest
with_phase_response_noise(ConversionSimulationRequest request,
                          const std::vector<ConversionPhaseResponseGroup> &groups,
                          double generation_reference_ohms = 50.) {
    if (request.devices.empty() || request.devices.size() > 512 || groups.empty() ||
        groups.size() > 512 || !std::isfinite(generation_reference_ohms) ||
        generation_reference_ohms <= 0.) {
        throw std::invalid_argument("invalid phase response request or generation reference");
    }
    using Key = std::tuple<std::size_t, std::size_t, int>;
    const double spacing = request.devices.front().model.spacing_hz();
    FrequencyConversionNetwork network(spacing);
    for (const auto &device : request.devices) {
        network.add_power_waves(device);
    }
    std::set<std::pair<std::size_t, std::size_t>> connected;
    for (const auto &wire : request.connections) {
        network.connect(wire.first_device, wire.first_port, wire.second_device, wire.second_port);
        connected.emplace(wire.first_device, wire.first_port);
        connected.emplace(wire.second_device, wire.second_port);
    }
    std::map<Key, std::size_t> lookup;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> ports;
    std::vector<ConversionChannel> channels;
    std::vector<Complex> source, reflection, references;
    std::vector<std::size_t> external;
    for (std::size_t d = 0; d < request.devices.size(); ++d) {
        const auto &device = request.devices[d];
        for (std::size_t i = 0; i < device.model.channels().size(); ++i) {
            const auto channel = device.model.channels()[i];
            const auto port = std::make_pair(d, channel.port);
            const auto global = channels.size();
            lookup.emplace(Key{d, channel.port, channel.bin}, global);
            if (!ports.count(port)) {
                ports.emplace(port, ports.size());
            }
            channels.push_back({ports.at(port), channel.bin});
            source.push_back(device.source[i]);
            reflection.push_back(device.reflection[i]);
            references.push_back(device.model.reference_impedances_ohms()[i]);
            if (!connected.count(port)) {
                external.push_back(global);
            }
        }
    }
    const auto n = channels.size();
    const auto zero = conversion_detail::zero(n);
    const FrequencyConversionModel original(spacing, channels, zero, zero, references);
    const std::vector<Complex> common_refs(n, generation_reference_ohms);
    const FrequencyConversionModel common(spacing, channels, zero, zero, common_refs);
    // Validate the existing process independently, including forbidden internal
    // noise; cancellation with a newly generated process cannot conceal it.
    const auto existing = request.additional_source_noise.value_or(original.zero_noise());
    (void)renormalize_conversion_boundary(
        original, references, external, source, reflection, existing);
    const auto boundary = renormalize_conversion_boundary(
        original, common_refs, external, source, reflection, original.zero_noise());
    auto generated = common.zero_noise();
    std::set<std::string> names;
    for (const auto &group : groups) {
        if (group.name.empty() || group.name.size() > 1024 ||
            group.name.find('\0') != std::string::npos || !names.insert(group.name).second) {
            throw std::invalid_argument("invalid or duplicate phase response group name");
        }
        std::vector<PhaseResponseCarrier> carriers;
        for (const auto &member : group.members) {
            const auto &c = member.carrier;
            const auto found = lookup.find({c.device, c.port, c.bin});
            if (found == lookup.end() || connected.count({c.device, c.port})) {
                throw std::invalid_argument(
                    "phase response carrier needs a known external channel");
            }
            carriers.push_back({found->second, boundary.source[found->second]});
        }
        const auto contribution = phase_response_group(channels, carriers, spacing, group.points);
        for (std::size_t i = 0; i < n * n; ++i) {
            phase_response_detail::add(generated.covariance.values[i],
                                       contribution.covariance.values[i]);
            phase_response_detail::add(generated.complementary.values[i],
                                       contribution.complementary.values[i]);
        }
    }
    auto local = renormalize_conversion_boundary(
                     common, references, external, boundary.source, boundary.reflection, generated)
                     .source_noise;
    for (std::size_t i = 0; i < n * n; ++i) {
        phase_response_detail::add(local.covariance.values[i], existing.covariance.values[i]);
        phase_response_detail::add(local.complementary.values[i], existing.complementary.values[i]);
    }
    original.validate_noise(local);
    request.additional_source_noise = std::move(local);
    return request;
}
} // namespace rfmodel
