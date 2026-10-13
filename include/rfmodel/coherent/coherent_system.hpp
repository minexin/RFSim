#pragma once
#include "rfmodel/coherent/coherent_pipeline.hpp"
#include "rfmodel/coherent/source_coherence.hpp"
#include <string>

namespace rfmodel {

class CoherentSystemCancelled : public std::runtime_error {
public:
    CoherentSystemCancelled() : std::runtime_error("coherent system execution cancelled") {
    }
};

// Host-facing, reusable feed-forward graph. Definition and execution are separate:
// compiled plans own model values and never borrow the builder or an input stream.
// A plan can run concurrently when its network/cancellation callbacks permit it.
class CoherentSystem {
    struct Owner {};

public:
    class Stream {
        friend class CoherentSystem;
        std::shared_ptr<const Owner> owner_;
        std::size_t index_;

        Stream(std::shared_ptr<const Owner> owner, std::size_t index)
            : owner_(std::move(owner)), index_(index) {
        }

    public:
        Stream(const Stream &) = default;
        Stream &operator=(const Stream &) = default;
    };

    struct PortInput {
        Stream stream;
        std::size_t port;
    };

    struct PortOutput {
        std::string name;
        std::size_t port;
    };

    struct MixerBranch {
        std::string output_name;
        Stream input;
        MixerDrive drive;
    };

    struct StageStatistics {
        std::optional<double> input_power_w;
        std::optional<double> common_amplitude_gain;
        std::optional<AmplifierOperatingPoint> operating_point;
        std::optional<PolynomialCounts> polynomial;
        std::size_t direct_contribution_count{}, discarded_direct_contributions{};
    };

    struct StageResult {
        std::string name;
        std::vector<std::string> output_names;
        StageStatistics statistics;
    };

private:
    using RuntimeStreams = std::vector<CoherentPipeline::Stream>;
    using Action = std::function<StageStatistics(CoherentPipeline &, RuntimeStreams &)>;

    struct Stage {
        std::string name;
        std::vector<std::string> outputs;
        Action action;
    };

    struct Definition {
        double spacing, reference;
        std::uint64_t reserved_group_max{1};
        std::map<std::string, std::uint64_t> source_groups;
        std::map<std::string, std::size_t> streams;
        std::set<std::string> stage_names;
        std::vector<Stage> stages;
        std::size_t transformations{};
    };

    std::shared_ptr<const Owner> owner_ = std::make_shared<Owner>();
    std::shared_ptr<const Definition> definition_;

    static void label(const std::string &value) {
        if (value.empty() || value.size() > 1024 || value.find('\0') != std::string::npos) {
            throw std::invalid_argument("invalid coherent system label");
        }
    }

    void validate_drive(const MixerDrive &drive) const {
        if (definition_->source_groups.empty() || drive.lo_coherence_group == 0 ||
            drive.lo_coherence_group > definition_->reserved_group_max) {
            throw std::invalid_argument("coherent mixer requires a declared LO group");
        }
        IdealRealMixer("coherent system mixer",
                       drive.lo_bin,
                       drive.conversion_gain_db,
                       drive.lo_phase_radians,
                       definition_->reference)
            .transmit({definition_->spacing, {}});
    }

    std::size_t index(Stream stream) const {
        if (stream.owner_ != owner_ || stream.index_ >= definition_->streams.size()) {
            throw std::invalid_argument("stream belongs to another coherent system");
        }
        return stream.index_;
    }

    std::vector<Stream> append(std::string name,
                               std::vector<std::string> outputs,
                               Action action,
                               std::size_t transformations) {
        label(name);
        if (outputs.empty() || outputs.size() > 4096 - definition_->streams.size() ||
            transformations > 512 - definition_->transformations) {
            throw std::length_error("coherent system stage/stream limit exceeded");
        }
        auto next = std::make_shared<Definition>(*definition_);
        if (!next->stage_names.insert(name).second) {
            throw std::invalid_argument("duplicate coherent system stage name");
        }
        std::vector<Stream> handles;
        handles.reserve(outputs.size());
        for (const auto &output : outputs) {
            label(output);
            const auto ordinal = next->streams.size();
            if (!next->streams.emplace(output, ordinal).second) {
                throw std::invalid_argument("duplicate coherent system stream name");
            }
            handles.push_back(Stream(owner_, ordinal));
        }
        next->transformations += transformations;
        next->stages.push_back({std::move(name), std::move(outputs), std::move(action)});
        // All throwing copies, labels and handles are complete before publication.
        definition_ = std::move(next);
        return handles;
    }

public:
    class Result {
        friend class CoherentSystem;
        std::shared_ptr<const Owner> owner_;
        std::shared_ptr<const Definition> definition_;
        std::shared_ptr<const CoherentPipeline> pipeline_;
        RuntimeStreams streams_;
        std::vector<StageResult> stages_;

        Result(std::shared_ptr<const Owner> owner,
               std::shared_ptr<const Definition> definition,
               std::shared_ptr<const CoherentPipeline> pipeline,
               RuntimeStreams streams,
               std::vector<StageResult> stages)
            : owner_(std::move(owner)), definition_(std::move(definition)),
              pipeline_(std::move(pipeline)), streams_(std::move(streams)),
              stages_(std::move(stages)) {
        }

    public:
        const StreamSnapshot &stream(Stream handle) const {
            if (handle.owner_ != owner_ || handle.index_ >= streams_.size()) {
                throw std::invalid_argument("stream is not in this coherent system snapshot");
            }
            return pipeline_->inspect(streams_[handle.index_]);
        }

        const StreamSnapshot &stream(const std::string &name) const {
            return pipeline_->inspect(streams_.at(definition_->streams.at(name)));
        }

        const std::vector<StageResult> &stages() const noexcept {
            return stages_;
        }

        PipelineUsage usage() const noexcept {
            return pipeline_->usage();
        }

        RootInfo history_root(std::uint64_t id) const {
            return pipeline_->history_root(id);
        }

        RootInfo physical_root(std::uint64_t id) const {
            return pipeline_->physical_root(id);
        }
    };

    class Plan {
        friend class CoherentSystem;
        std::shared_ptr<const Owner> owner_;
        std::shared_ptr<const Definition> definition_;

        Plan(std::shared_ptr<const Owner> owner, std::shared_ptr<const Definition> definition)
            : owner_(std::move(owner)), definition_(std::move(definition)) {
        }

    public:
        Result execute(const std::function<bool()> &cancelled = {}) const {
            auto pipeline = std::make_shared<CoherentPipeline>(
                definition_->spacing, definition_->reference, definition_->reserved_group_max);
            RuntimeStreams streams;
            streams.reserve(definition_->streams.size());
            std::vector<StageResult> stages;
            stages.reserve(definition_->stages.size());
            for (const auto &stage : definition_->stages) {
                if (cancelled && cancelled()) {
                    throw CoherentSystemCancelled();
                }
                auto statistics = stage.action(*pipeline, streams);
                stages.push_back({stage.name, stage.outputs, std::move(statistics)});
            }
            return Result(
                owner_, definition_, std::move(pipeline), std::move(streams), std::move(stages));
        }

        std::size_t stream_count() const noexcept {
            return definition_->streams.size();
        }

        std::size_t stage_count() const noexcept {
            return definition_->stages.size();
        }
    };

    CoherentSystem(double spacing_hz,
                   double reference_ohms,
                   const std::vector<SourceCoherence> &sources) {
        const auto groups = assign_source_coherence(sources);
        auto data = std::make_shared<Definition>();
        data->spacing = spacing_hz;
        data->reference = reference_ohms;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            data->source_groups.emplace(sources[i].source_id, groups[i]);
            data->reserved_group_max = std::max(data->reserved_group_max, groups[i]);
        }
        CoherentPipeline validate(spacing_hz, reference_ohms, data->reserved_group_max);
        definition_ = std::move(data);
    }

    CoherentSystem(const CoherentSystem &) = delete;
    CoherentSystem &operator=(const CoherentSystem &) = delete;
    CoherentSystem(CoherentSystem &&) = delete;
    CoherentSystem &operator=(CoherentSystem &&) = delete;

    std::uint64_t source_group(const std::string &name) const {
        return definition_->source_groups.at(name);
    }

    Plan compile() const {
        return Plan(owner_, definition_);
    }

    Stream input(std::string name, std::vector<CoherentComponent> sources) {
        // Validate before changing the graph, including undeclared group IDs.
        reduce_coherent_components(definition_->spacing, sources);
        for (const auto &source : sources) {
            if (source.kind != SpectrumKind::source || source.coherence_group == 0 ||
                definition_->source_groups.empty() ||
                source.coherence_group > definition_->reserved_group_max) {
                throw std::invalid_argument(
                    "coherent system inputs require declared source groups");
            }
        }
        const auto output = name;
        return append(
                   std::move(name),
                   {output},
                   [sources = std::move(sources)](auto &pipeline, auto &streams) {
                       streams.push_back(pipeline.seed(sources));
                       return StageStatistics{};
                   },
                   0)
            .front();
    }

    Stream mixer(std::string output, Stream input, MixerDrive drive) {
        validate_drive(drive);
        const auto selected = index(input);
        const auto stage = output;
        return append(
                   stage,
                   {std::move(output)},
                   [selected, drive](auto &pipeline, auto &streams) {
                       streams.push_back(pipeline.mix(streams.at(selected), drive));
                       return StageStatistics{};
                   },
                   1)
            .front();
    }

    std::vector<Stream> mixer_bank(std::string stage, const std::vector<MixerBranch> &branches) {
        if (branches.empty() || branches.size() > 2048) {
            throw std::invalid_argument("invalid mixer bank size");
        }
        std::vector<std::string> names;
        std::vector<std::pair<std::size_t, MixerDrive>> selected;
        for (const auto &branch : branches) {
            validate_drive(branch.drive);
            names.push_back(branch.output_name);
            selected.emplace_back(index(branch.input), branch.drive);
        }
        return append(
            std::move(stage),
            std::move(names),
            [selected = std::move(selected)](auto &pipeline, auto &streams) {
                std::vector<CoherentPipeline::MixerBranch> inputs;
                for (const auto &branch : selected) {
                    inputs.push_back({streams.at(branch.first), branch.second});
                }
                auto result = pipeline.mixer_bank(inputs);
                streams.insert(streams.end(), result.begin(), result.end());
                return StageStatistics{};
            },
            1);
    }

    std::vector<Stream> linear_network(std::string stage,
                                       const std::vector<PortInput> &inputs,
                                       std::vector<std::size_t> external_ports,
                                       const std::vector<PortOutput> &outputs,
                                       CoherentPipeline::NetworkBuilder build) {
        if (!build || inputs.size() > 4096 || outputs.empty() || outputs.size() > 1024) {
            throw std::invalid_argument("invalid coherent system linear stage");
        }
        std::vector<std::pair<std::size_t, std::size_t>> selected;
        for (const auto &input : inputs) {
            selected.emplace_back(index(input.stream), input.port);
        }
        std::vector<std::string> names;
        std::vector<std::size_t> ports;
        for (const auto &output : outputs) {
            names.push_back(output.name);
            ports.push_back(output.port);
        }
        return append(
            std::move(stage),
            std::move(names),
            [selected = std::move(selected),
             external_ports = std::move(external_ports),
             ports = std::move(ports),
             build = std::move(build)](auto &pipeline, auto &streams) {
                std::vector<CoherentPipeline::PortStream> incident;
                for (const auto &input : selected) {
                    incident.push_back({streams.at(input.first), input.second});
                }
                auto result =
                    pipeline.linear_network_outputs(incident, external_ports, ports, build);
                streams.insert(streams.end(), result.begin(), result.end());
                return StageStatistics{};
            },
            1);
    }

    Stream polynomial(std::string output,
                      Stream input,
                      std::vector<double> coefficients,
                      std::size_t max_source_order) {
        const auto selected = index(input);
        const auto stage = output;
        return append(
                   stage,
                   {std::move(output)},
                   [selected, coefficients = std::move(coefficients), max_source_order](
                       auto &pipeline, auto &streams) {
                       auto result = pipeline.polynomial(
                           streams.at(selected), coefficients, max_source_order);
                       streams.push_back(result.output);
                       StageStatistics stats;
                       stats.polynomial = std::move(result.counts);
                       return stats;
                   },
                   1)
            .front();
    }

    Stream fundamental_compression(std::string output,
                                   Stream input,
                                   SaturatingFundamentalCompression model) {
        const auto selected = index(input);
        const auto stage = output;
        return append(
                   stage,
                   {std::move(output)},
                   [selected, model = std::move(model)](auto &pipeline, auto &streams) {
                       const auto result =
                           pipeline.compress_fundamentals(streams.at(selected), model);
                       streams.push_back(result.output);
                       StageStatistics stats;
                       stats.input_power_w = result.input_power_w;
                       stats.common_amplitude_gain = result.common_amplitude_gain;
                       return stats;
                   },
                   1)
            .front();
    }

    Stream limited_amplifier(std::string output,
                             Stream input,
                             CoherentLimitedAmplifier model,
                             AmplifierInputMode mode = AmplifierInputMode::sources_only) {
        const auto selected = index(input);
        const auto stage = output;
        return append(
                   stage,
                   {std::move(output)},
                   [selected, model = std::move(model), mode](auto &pipeline, auto &streams) {
                       auto result = pipeline.limited_amplifier(streams.at(selected), model, mode);
                       streams.push_back(result.output);
                       return StageStatistics{result.input_power_w,
                                              {},
                                              result.operating_point,
                                              std::move(result.nonlinear_counts),
                                              result.direct_contribution_count,
                                              result.discarded_direct_contributions};
                   },
                   1)
            .front();
    }

    Stream highorder_amplifier(std::string output,
                               Stream input,
                               CoherentHighOrderAmplifier model,
                               std::size_t max_source_order,
                               AmplifierInputMode mode = AmplifierInputMode::sources_only) {
        const auto selected = index(input);
        const auto stage = output;
        return append(
                   stage,
                   {std::move(output)},
                   [selected, model = std::move(model), max_source_order, mode](auto &pipeline,
                                                                                auto &streams) {
                       auto result = pipeline.highorder_amplifier(
                           streams.at(selected), model, max_source_order, mode);
                       streams.push_back(result.output);
                       return StageStatistics{result.input_power_w,
                                              {},
                                              result.operating_point,
                                              std::move(result.nonlinear_counts),
                                              result.direct_contribution_count,
                                              result.discarded_direct_contributions};
                   },
                   1)
            .front();
    }
};
} // namespace rfmodel
