#pragma once
#include "rfmodel/conversion/conversion_frequency_plan.hpp"
#include <optional>

namespace rfmodel {
struct ConversionEndpoint {
    std::size_t device{}, port{};
    int bin{};
};

struct GeneratedConversionBoundary {
    Complex reflection{};
    std::optional<double> noise_w_per_hz, temperature_k;
};

struct ConversionSeedState {
    // Empty vectors/noise optionals mean zero/default; otherwise cover all seeds.
    std::vector<Complex> source, reflection, output_offset, initial_incident, references;
    std::optional<ConversionNoise> source_noise, intrinsic_noise;
    std::vector<std::size_t> selected_output_channels;
};

struct ExpandedConversionState {
    std::vector<Complex> source, reflection, output_offset, initial_incident, references;
    ConversionNoise source_noise, intrinsic_noise;
    std::vector<std::size_t> selected_output_channels;
};

// Structural closure plus the seed->expanded transport previously owned by the
// Python adapter. Data is scattered by channel identity, never by assumed order.
class ConversionGrid {
    double spacing_;
    ConversionFrequencyPlan plan_;
    std::vector<std::size_t> offsets_, global_mapping_;
    std::vector<ConversionChannel> global_channels_;
    std::set<std::pair<std::size_t, std::size_t>> connected_;

    static std::vector<Complex> scatter(const std::vector<Complex> &values,
                                        const std::vector<std::size_t> &mapping,
                                        const std::vector<ConversionChannel> &channels,
                                        Complex fill,
                                        bool dc_real) {
        if (values.size() != mapping.size()) {
            throw std::invalid_argument("seed vector dimensions differ");
        }
        std::vector<Complex> result(channels.size(), fill);
        for (std::size_t i = 0; i < mapping.size(); ++i) {
            result[mapping[i]] = values[i];
        }
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (!conversion_detail::finite(result[i]) ||
                (dc_real && channels[i].bin == 0 && result[i].imag() != 0.)) {
                throw std::invalid_argument("nonfinite seed value or complex DC wave");
            }
        }
        return result;
    }

    ConversionNoise scatter_noise(const ConversionNoise &noise,
                                  const std::vector<std::size_t> &mapping,
                                  const std::vector<ConversionChannel> &channels) const {
        const auto old = mapping.size(), n = channels.size();
        ConversionNoise result{conversion_detail::zero(n), conversion_detail::zero(n)};
        for (const auto *matrix : {&noise.covariance, &noise.complementary}) {
            if (matrix->ports != old || matrix->values.size() != old * old) {
                throw std::invalid_argument("seed noise dimensions differ");
            }
            for (auto value : matrix->values) {
                if (!conversion_detail::finite(value)) {
                    throw std::invalid_argument("nonfinite seed noise");
                }
            }
        }
        for (std::size_t i = 0; i < old; ++i) {
            for (std::size_t j = 0; j < old; ++j) {
                result.covariance(mapping[i], mapping[j]) = noise.covariance(i, j);
                result.complementary(mapping[i], mapping[j]) = noise.complementary(i, j);
            }
        }
        FrequencyConversionModel validation(
            spacing_, channels, conversion_detail::zero(n), conversion_detail::zero(n));
        validation.validate_noise(result);
        return result;
    }

    std::vector<std::size_t> order(std::size_t device,
                                   const std::vector<ConversionChannel> &original) const {
        const auto &target = plan_.channels.at(device);
        if (original.size() != target.size()) {
            throw std::invalid_argument("model channels differ from planned channels");
        }
        std::map<std::pair<std::size_t, int>, std::size_t> indices;
        for (std::size_t i = 0; i < original.size(); ++i) {
            if (!indices.emplace(std::make_pair(original[i].port, original[i].bin), i).second) {
                throw std::invalid_argument("duplicate model channel");
            }
        }
        std::vector<std::size_t> result;
        for (auto channel : target) {
            const auto found = indices.find({channel.port, channel.bin});
            if (found == indices.end()) {
                throw std::invalid_argument("model port/grid differs from frequency declaration");
            }
            result.push_back(found->second);
        }
        return result;
    }

public:
    ConversionGrid(double spacing_hz,
                   const std::vector<ConversionFrequencyDevice> &devices,
                   const std::vector<ConversionPortConnection> &connections,
                   const ConversionFrequencyOptions &options = {})
        : spacing_(spacing_hz),
          plan_(plan_conversion_frequencies(spacing_hz, devices, connections, options)) {
        std::map<std::pair<std::size_t, std::size_t>, std::size_t> ports;
        for (std::size_t d = 0; d < plan_.channels.size(); ++d) {
            offsets_.push_back(global_channels_.size());
            for (auto index : plan_.seed_channel_indices[d]) {
                global_mapping_.push_back(offsets_.back() + index);
            }
            for (auto channel : plan_.channels[d]) {
                const auto port = std::make_pair(d, channel.port);
                if (!ports.count(port)) {
                    ports.emplace(port, ports.size());
                }
                global_channels_.push_back({ports.at(port), channel.bin});
            }
        }
        for (const auto &wire : connections) {
            connected_.emplace(wire.first_device, wire.first_port);
            connected_.emplace(wire.second_device, wire.second_port);
        }
    }

    const ConversionFrequencyPlan &plan() const noexcept {
        return plan_;
    }

    const std::vector<std::size_t> &global_seed_indices() const noexcept {
        return global_mapping_;
    }

    std::size_t channel_offset(std::size_t device) const {
        return offsets_.at(device);
    }

    std::vector<Complex> local_vector(std::size_t device, const std::vector<Complex> &seeds) const {
        return scatter(
            seeds, plan_.seed_channel_indices.at(device), plan_.channels.at(device), {}, true);
    }

    std::vector<Complex> global_vector(const std::vector<Complex> &seeds) const {
        return scatter(seeds, global_mapping_, global_channels_, {}, true);
    }

    std::vector<Complex> local_references(std::size_t device,
                                          const std::vector<Complex> &seeds,
                                          double generated_reference_ohms = 50.) const {
        if (!std::isfinite(generated_reference_ohms) || generated_reference_ohms <= 0.) {
            throw std::invalid_argument("generated channel reference must be positive and finite");
        }
        auto result = scatter(seeds,
                              plan_.seed_channel_indices.at(device),
                              plan_.channels.at(device),
                              generated_reference_ohms,
                              true);
        for (auto value : result) {
            if (value.real() <= 0.) {
                throw std::invalid_argument("channel reference must have positive real part");
            }
        }
        return result;
    }

    ConversionNoise local_noise(std::size_t device, const ConversionNoise &seeds) const {
        return scatter_noise(
            seeds, plan_.seed_channel_indices.at(device), plan_.channels.at(device));
    }

    ConversionNoise global_noise(const ConversionNoise &seeds) const {
        return scatter_noise(seeds, global_mapping_, global_channels_);
    }

    std::vector<std::size_t> local_indices(std::size_t device,
                                           const std::vector<std::size_t> &seeds) const {
        std::set<std::size_t> unique;
        std::vector<std::size_t> result;
        const auto &mapping = plan_.seed_channel_indices.at(device);
        for (auto index : seeds) {
            if (index >= mapping.size() || !unique.insert(index).second) {
                throw std::invalid_argument("invalid or repeated selected seed channel");
            }
            result.push_back(mapping[index]);
        }
        return result;
    }

    std::vector<ConversionEndpoint> generated_external_channels() const {
        std::vector<ConversionEndpoint> result;
        for (std::size_t d = 0; d < plan_.channels.size(); ++d) {
            const auto &mapping = plan_.seed_channel_indices[d];
            const std::set<std::size_t> seeds(mapping.begin(), mapping.end());
            for (std::size_t i = 0; i < plan_.channels[d].size(); ++i) {
                auto c = plan_.channels[d][i];
                if (!seeds.count(i) && !connected_.count({d, c.port})) {
                    result.push_back({d, c.port, c.bin});
                }
            }
        }
        return result;
    }

    ExpandedConversionState
    expand(std::size_t device,
           const ConversionSeedState &seeds,
           double generated_reference_ohms = 50.,
           const std::optional<GeneratedConversionBoundary> &boundary = {}) const {
        const auto n = plan_.channels.at(device).size();
        const auto seed_count = plan_.seed_channel_indices.at(device).size();
        auto waves = [&](const auto &value) {
            return value.empty() ? std::vector<Complex>(n) : local_vector(device, value);
        };
        const auto zero = ConversionNoise{conversion_detail::zero(n), conversion_detail::zero(n)};
        ExpandedConversionState result{
            waves(seeds.source),
            waves(seeds.reflection),
            waves(seeds.output_offset),
            waves(seeds.initial_incident),
            local_references(device,
                             seeds.references.empty()
                                 ? std::vector<Complex>(seed_count, generated_reference_ohms)
                                 : seeds.references,
                             generated_reference_ohms),
            seeds.source_noise ? local_noise(device, *seeds.source_noise) : zero,
            seeds.intrinsic_noise ? local_noise(device, *seeds.intrinsic_noise) : zero,
            local_indices(device, seeds.selected_output_channels)};
        if (boundary) {
            if (!conversion_detail::finite(boundary->reflection) ||
                (boundary->noise_w_per_hz && boundary->temperature_k)) {
                throw std::invalid_argument(
                    "generated boundary requires one finite noise specification");
            }
            double density = boundary->noise_w_per_hz.value_or(0.);
            if (boundary->temperature_k) {
                if (!std::isfinite(*boundary->temperature_k) || *boundary->temperature_k < 0. ||
                    std::abs(boundary->reflection) > 1.) {
                    throw std::invalid_argument("generated thermal boundary must be passive");
                }
                density = 1.380649e-23 * *boundary->temperature_k *
                          std::max(0., 1. - std::norm(boundary->reflection));
            }
            if (!std::isfinite(density) || density < 0.) {
                throw std::invalid_argument("invalid generated noise density");
            }
            const auto &mapping = plan_.seed_channel_indices[device];
            const std::set<std::size_t> selected(mapping.begin(), mapping.end());
            for (std::size_t i = 0; i < n; ++i) {
                const auto c = plan_.channels[device][i];
                if (selected.count(i) || connected_.count({device, c.port})) {
                    continue;
                }
                if (c.bin == 0 && boundary->reflection.imag() != 0.) {
                    throw std::invalid_argument("complex generated DC reflection");
                }
                result.reflection[i] = boundary->reflection;
                result.source_noise.covariance(i, i) = density;
                if (c.bin == 0) {
                    result.source_noise.complementary(i, i) = density;
                }
            }
        }
        return result;
    }

    // Align a generated model (e.g. bin-major linear lifting) and every local
    // vector/matrix to the plan's port-major order. No coordinate conversion.
    ConversionDevice align_device(std::size_t device, const ConversionDevice &input) const {
        const auto permutation = order(device, input.model.channels());
        const auto n = permutation.size();
        if (input.model.spacing_hz() != spacing_) {
            throw std::invalid_argument("planned model spacing differs");
        }
        auto vector = [&](const std::vector<Complex> &original) {
            if (original.size() != n) {
                throw std::invalid_argument("model vector dimensions differ");
            }
            std::vector<Complex> result;
            for (auto i : permutation) {
                result.push_back(original[i]);
            }
            return result;
        };
        auto matrix = [&](const SMatrix &original) {
            if (original.ports != n || original.values.size() != n * n) {
                throw std::invalid_argument("model matrix dimensions differ");
            }
            auto result = conversion_detail::zero(n);
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    result(i, j) = original(permutation[i], permutation[j]);
                }
            }
            return result;
        };
        ConversionDevice result{
            FrequencyConversionModel(spacing_,
                                     plan_.channels[device],
                                     matrix(input.model.direct()),
                                     matrix(input.model.conjugate()),
                                     vector(input.model.reference_impedances_ohms())),
            vector(input.source),
            vector(input.reflection),
            {matrix(input.source_noise.covariance), matrix(input.source_noise.complementary)},
            {matrix(input.intrinsic_noise.covariance),
             matrix(input.intrinsic_noise.complementary)}};
        result.model.validate_noise(result.source_noise);
        result.model.validate_noise(result.intrinsic_noise);
        return result;
    }

    std::vector<Complex> align_vector(std::size_t device,
                                      const std::vector<ConversionChannel> &original_channels,
                                      const std::vector<Complex> &values) const {
        const auto permutation = order(device, original_channels);
        if (values.size() != permutation.size()) {
            throw std::invalid_argument("model vector dimensions differ");
        }
        std::vector<Complex> result;
        for (auto i : permutation) {
            result.push_back(values[i]);
        }
        for (auto value : result) {
            if (!conversion_detail::finite(value)) {
                throw std::invalid_argument("nonfinite model vector");
            }
        }
        return result;
    }
};
} // namespace rfmodel
