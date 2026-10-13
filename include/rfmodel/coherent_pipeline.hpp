#pragma once
#include "coherent_mixer.hpp"
#include "coherent_network.hpp"
#include "coherent_polynomial.hpp"
#include "origin_expression.hpp"
#include "saturating_fundamental.hpp"
#include <iterator>
#include <memory>
#include <utility>

namespace rfmodel {
enum class RootRole {
    rf,
    lo,
    mixed
};

struct RootInfo {
    std::uint64_t id{};
    std::uint64_t coherence_group{};
    int bin{};
    double bandwidth_hz{};
    RootRole role{};
};

struct PhysicalIdentity {
    MixingOrigin factors;
};

struct StreamRecord {
    CoherentComponent component;
    PhysicalIdentity physical_identity;
    OriginExpression history;
};

struct StreamSnapshot {
    std::vector<StreamRecord> records;
    CoherentReduction reduction;
};

struct MixerDrive {
    int lo_bin{};
    std::uint64_t lo_coherence_group{};
    double conversion_gain_db{};
    double lo_phase_radians{};
};

struct PolynomialCounts {
    std::size_t physical_input_count{};
    std::size_t expanded_input_count{};
    std::size_t generated_term_count{};
    std::size_t discarded_term_count{};
    std::map<std::size_t, std::size_t> discarded_by_source_order;
};

struct PipelineUsage {
    std::size_t stream_count{};
    std::size_t transformation_count{};
    std::size_t physical_record_count{};
    std::size_t history_term_count{};
    std::size_t history_factor_count{};
    std::size_t physical_root_count{};
    std::size_t history_root_count{};
    std::size_t physical_identity_factor_count{};
};

// Immutable, bounded feed-forward streams. Physical identities determine power;
// stream-local histories retain cancelling paths and full source provenance.
// Contexts are not thread-safe. Network callbacks should be pure builders:
// library state rolls back on failure, but callback side effects cannot roll back.
class CoherentPipeline {
    struct Owner {};

public:
    class Stream {
        friend class CoherentPipeline;
        // A stale handle keeps its token alive, preventing allocator address
        // reuse from making it valid for a later context at the same address.
        std::shared_ptr<const Owner> owner_;
        std::size_t index_;

        Stream(std::shared_ptr<const Owner> owner, std::size_t index)
            : owner_(std::move(owner)), index_(index) {
        }

    public:
        Stream(const Stream &) = default;
        Stream &operator=(const Stream &) = default;
    };

    struct PortStream {
        Stream stream;
        std::size_t input_port;
    };

    struct PolynomialResult {
        Stream output;
        PolynomialCounts counts;
    };

    struct CompressionResult {
        Stream output;
        double input_power_w;
        double common_amplitude_gain;
    };

    using NetworkBuilder = std::function<LinearNetwork(double)>;

private:
    using RootKey = std::tuple<std::uint64_t, int, double, RootRole>;
    using ComponentKey = std::tuple<int, SpectrumKind, double, std::uint64_t>;

    struct IdentityKey {
        int bin;
        SpectrumKind kind;
        double bandwidth;
        MixingOrigin factors;

        bool operator<(const IdentityKey &other) const {
            const auto left = std::tie(bin, kind, bandwidth);
            const auto right = std::tie(other.bin, other.kind, other.bandwidth);
            return left != right ? left < right
                                 : origin_expression_detail::Less{}(factors, other.factors);
        }
    };

    struct Roots {
        std::map<RootKey, std::uint64_t> indices;
        std::vector<RootInfo> entries;

        std::uint64_t intern(std::uint64_t group, int bin, double bandwidth, RootRole role) {
            const RootKey key{group, bin, bandwidth, role};
            const auto found = indices.find(key);
            if (found != indices.end()) {
                return found->second;
            }
            if (entries.size() >= 65536) {
                throw std::length_error("coherent pipeline root limit exceeded");
            }
            const auto id = static_cast<std::uint64_t>(entries.size() + 1);
            entries.push_back({id, group, bin, bandwidth, role});
            indices.emplace(key, id);
            return id;
        }

        RootInfo at(std::uint64_t id) const {
            if (id == 0 || id > entries.size()) {
                throw std::out_of_range("coherent pipeline root ID");
            }
            return entries[static_cast<std::size_t>(id - 1)];
        }
    };

    struct State {
        std::uint64_t highest_group;
        Roots physical;
        Roots history;
        std::map<IdentityKey, std::uint64_t> identities;
        std::map<ComponentKey, MixingOrigin> assignments;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> mixed_groups;
        PipelineUsage usage;
    };

    struct Flattened {
        std::vector<CoherentComponent> components;
        std::map<std::uint64_t, StreamRecord> lookup;
    };

    class Operation {
        bool &active_;

    public:
        explicit Operation(bool &active) : active_(active) {
            if (active_) {
                throw std::logic_error("reentrant coherent pipeline mutation");
            }
            active_ = true;
        }

        ~Operation() {
            active_ = false;
        }

        Operation(const Operation &) = delete;
        Operation &operator=(const Operation &) = delete;
    };

    bool active_ = false;
    double spacing_;
    double reference_;
    std::uint64_t reserved_;
    std::shared_ptr<const Owner> owner_ = std::make_shared<Owner>();
    std::unique_ptr<State> state_;
    std::vector<std::unique_ptr<const StreamSnapshot>> streams_;

    static void
    add_bounded(std::size_t &count, std::size_t addition, std::size_t limit, const char *message) {
        if (addition > limit - count) {
            throw std::length_error(message);
        }
        count += addition;
    }

    static std::uint64_t allocate_group(State &state) {
        if (state.highest_group == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("coherent pipeline group IDs exhausted");
        }
        return ++state.highest_group;
    }

    void source_group(std::uint64_t group) const {
        if (group == 0 || group > reserved_) {
            throw std::invalid_argument("source group outside reserved pipeline namespace");
        }
    }

    static ComponentKey key(const CoherentComponent &component) {
        return {component.bin, component.kind, component.bandwidth_hz, component.coherence_group};
    }

    static bool close_width(double a, double b) {
        return std::abs(a - b) <= 1e-12 * std::max(std::abs(a), std::abs(b));
    }

    static std::pair<long long, double> support(const MixingOrigin &factors, const Roots &roots) {
        long long bin = 0;
        coherence_detail::Sum width;
        for (const auto &factor : factors) {
            const auto root = roots.at(factor.root_id);
            bin += static_cast<long long>(factor.sign) * root.bin;
            width.add(root.bandwidth_hz);
        }
        return {bin, width.result()};
    }

    static void validate_support(const CoherentComponent &component,
                                 const std::pair<long long, double> &value) {
        if (value.first != component.bin || !close_width(value.second, component.bandwidth_hz)) {
            throw std::invalid_argument("coherent pipeline origin support differs from component");
        }
    }

    static std::unique_ptr<State> begin(const State &state, bool transformation) {
        if (state.usage.stream_count >= 4096 ||
            (transformation && state.usage.transformation_count >= 512)) {
            throw std::length_error("coherent pipeline stream/operation limit exceeded");
        }
        auto work = std::make_unique<State>(state);
        if (transformation) {
            ++work->usage.transformation_count;
        }
        return work;
    }

    static void
    register_identity(State &state, CoherentComponent &component, const MixingOrigin &factors) {
        const IdentityKey identity{component.bin, component.kind, component.bandwidth_hz, factors};
        auto found = state.identities.find(identity);
        if (found == state.identities.end()) {
            if (component.coherence_group == 0) {
                component.coherence_group = allocate_group(state);
            }
            add_bounded(state.usage.physical_identity_factor_count,
                        factors.size(),
                        1048576,
                        "coherent pipeline physical identity factor limit exceeded");
            found = state.identities.emplace(identity, component.coherence_group).first;
        } else if (component.coherence_group != 0 && component.coherence_group != found->second) {
            throw std::invalid_argument("conflicting coherent pipeline identity group");
        }
        component.coherence_group = found->second;
        auto assigned = state.assignments.emplace(key(component), factors);
        if (!assigned.second && assigned.first->second != factors) {
            throw std::invalid_argument("conflicting coherent pipeline physical identity");
        }
    }

    Stream store(std::unique_ptr<State> work, std::vector<StreamRecord> records) {
        if (records.size() > 4096) {
            throw std::length_error("coherent pipeline physical record limit exceeded");
        }

        struct Group {
            CoherentComponent component;
            PhysicalIdentity identity;
            std::vector<OriginExpression> histories;
        };

        std::map<ComponentKey, Group> groups;
        for (auto &record : records) {
            auto &component = record.component;
            auto &factors = record.physical_identity.factors;
            factors = expand_mixing_origin({factors}, {1});
            const auto physical_support = support(factors, work->physical);
            validate_support(component, physical_support);
            component.bandwidth_hz = physical_support.second;
            for (const auto &term : record.history) {
                validate_support(component, support(term.factors, work->history));
            }
            register_identity(*work, component, factors);
            auto result = groups.emplace(key(component), Group{component, {factors}, {}});
            result.first->second.histories.push_back(std::move(record.history));
        }
        auto snapshot = std::make_unique<StreamSnapshot>();
        std::vector<CoherentComponent> components;
        for (auto &entry : groups) {
            auto &group = entry.second;
            auto history = sum_origin_expressions(group.histories);
            if (history.empty()) {
                throw std::invalid_argument("coherent pipeline record requires source history");
            }
            group.component.amplitude = origin_expression_amplitude(history);
            add_bounded(work->usage.history_term_count,
                        history.size(),
                        65536,
                        "coherent pipeline stored history term limit exceeded");
            for (const auto &term : history) {
                add_bounded(work->usage.history_factor_count,
                            term.factors.size(),
                            1048576,
                            "coherent pipeline stored history factor limit exceeded");
            }
            components.push_back(group.component);
            snapshot->records.push_back(
                {group.component, std::move(group.identity), std::move(history)});
        }
        add_bounded(work->usage.physical_record_count,
                    snapshot->records.size(),
                    65536,
                    "coherent pipeline stored physical record limit exceeded");
        snapshot->reduction = reduce_coherent_components(spacing_, components);
        ++work->usage.stream_count;
        work->usage.physical_root_count = work->physical.entries.size();
        work->usage.history_root_count = work->history.entries.size();
        // push_back is the last potentially throwing operation before commit.
        streams_.push_back(std::move(snapshot));
        state_.swap(work);
        return Stream(owner_, streams_.size() - 1);
    }

    static Flattened flatten(State &state, const StreamSnapshot &snapshot) {
        Flattened result;
        for (const auto &record : snapshot.records) {
            for (const auto &term : record.history) {
                if (result.components.size() >= 4096) {
                    throw std::length_error("coherent pipeline expanded input limit exceeded");
                }
                auto component = record.component;
                component.coherence_group = allocate_group(state);
                component.amplitude = term.amplitude;
                result.components.push_back(component);
                result.lookup.emplace(
                    component.coherence_group,
                    StreamRecord{record.component, record.physical_identity, {term}});
            }
        }
        return result;
    }

    static MixingOrigin compose(const std::vector<const StreamRecord *> &parents,
                                const std::vector<int> &indices,
                                bool physical) {
        std::vector<MixingOrigin> selected;
        std::vector<int> local;
        for (int index : indices) {
            const auto &record = *parents[static_cast<std::size_t>(std::abs(index) - 1)];
            selected.push_back(physical ? record.physical_identity.factors
                                        : record.history[0].factors);
            const auto position = static_cast<int>(selected.size());
            local.push_back(index < 0 ? -position : position);
        }
        return expand_mixing_origin(selected, local);
    }

public:
    CoherentPipeline(double spacing_hz,
                     double reference_ohms,
                     std::uint64_t reserved_source_group_max)
        : spacing_(spacing_hz), reference_(reference_ohms), reserved_(reserved_source_group_max),
          state_(std::make_unique<State>()) {
        reduce_coherent_components(spacing_, {});
        if (!std::isfinite(reference_) || reference_ <= 0 || reserved_ == 0) {
            throw std::invalid_argument("invalid coherent pipeline reference/reserved groups");
        }
        state_->highest_group = reserved_;
    }

    CoherentPipeline(const CoherentPipeline &) = delete;
    CoherentPipeline &operator=(const CoherentPipeline &) = delete;
    CoherentPipeline(CoherentPipeline &&) = delete;
    CoherentPipeline &operator=(CoherentPipeline &&) = delete;

    const StreamSnapshot &inspect(Stream stream) const {
        if (stream.owner_ != owner_ || stream.index_ >= streams_.size()) {
            throw std::invalid_argument("stream belongs to another coherent pipeline");
        }
        return *streams_[stream.index_];
    }

    RootInfo history_root(std::uint64_t id) const {
        return state_->history.at(id);
    }

    RootInfo physical_root(std::uint64_t id) const {
        return state_->physical.at(id);
    }

    PipelineUsage usage() const noexcept {
        return state_->usage;
    }

    Stream seed(const std::vector<CoherentComponent> &sources) {
        const Operation operation(active_);
        const auto input = reduce_coherent_components(spacing_, sources);
        auto work = begin(*state_, false);
        std::vector<StreamRecord> records;
        for (const auto &component : input.components) {
            source_group(component.coherence_group);
            if (component.kind != SpectrumKind::source) {
                throw std::invalid_argument("coherent pipeline seeds require source kind");
            }
            const auto physical = work->physical.intern(
                component.coherence_group, component.bin, component.bandwidth_hz, RootRole::rf);
            const auto history = work->history.intern(
                component.coherence_group, component.bin, component.bandwidth_hz, RootRole::rf);
            records.push_back(
                {component, {{{physical, 1}}}, {{{{history, 1}}, component.amplitude}}});
        }
        return store(std::move(work), std::move(records));
    }

    Stream mix(Stream input, const MixerDrive &lo) {
        const Operation operation(active_);
        const auto &snapshot = inspect(input);
        source_group(lo.lo_coherence_group);
        IdealRealMixer mixer("coherent pipeline mixer",
                             lo.lo_bin,
                             lo.conversion_gain_db,
                             lo.lo_phase_radians,
                             reference_);
        mixer.transmit({spacing_, {}}); // Validate even an empty stream.
        auto work = begin(*state_, true);
        std::vector<CoherentMixerInput> inputs;
        for (const auto &record : snapshot.records) {
            inputs.push_back({record.component,
                              lo.lo_bin,
                              lo.conversion_gain_db,
                              lo.lo_phase_radians,
                              lo.lo_coherence_group});
        }
        const auto converted = mix_coherent_components(spacing_, inputs, work->highest_group);
        std::vector<StreamRecord> output;
        if (!inputs.empty()) {
            const auto root =
                work->history.intern(lo.lo_coherence_group, lo.lo_bin, 0., RootRole::lo);
            const OriginExpression pump{{{{root, 1}}, std::polar(1., lo.lo_phase_radians)}};
            const Complex gain{std::pow(10., lo.conversion_gain_db / 20.), 0.};
            for (std::size_t i = 0; i < snapshot.records.size(); ++i) {
                const auto &record = snapshot.records[i];
                const auto pair =
                    std::make_pair(record.component.coherence_group, lo.lo_coherence_group);
                auto found = work->mixed_groups.find(pair);
                if (found == work->mixed_groups.end()) {
                    found = work->mixed_groups.emplace(pair, allocate_group(*work)).first;
                }
                for (std::size_t side = 0; side < 2; ++side) {
                    auto component = converted[2 * i + side];
                    component.coherence_group = found->second;
                    const auto physical = work->physical.intern(component.coherence_group,
                                                                component.bin,
                                                                component.bandwidth_hz,
                                                                RootRole::mixed);
                    const auto indices = side == 1 ? std::vector<int>{1, 2}
                                         : record.component.bin > lo.lo_bin
                                             ? std::vector<int>{1, -2}
                                             : std::vector<int>{-1, 2};
                    auto history =
                        product_origin_expressions({record.history, pump}, indices, gain);
                    output.push_back({component, {{{physical, 1}}}, std::move(history)});
                }
            }
        }
        return store(std::move(work), std::move(output));
    }

    Stream linear_network(const std::vector<PortStream> &inputs,
                          const std::vector<std::size_t> &external_ports,
                          std::size_t output_port,
                          const NetworkBuilder &build) {
        const Operation operation(active_);
        if (inputs.size() > 4096) {
            throw std::length_error("coherent pipeline port stream limit exceeded");
        }
        if (!build || external_ports.empty() || external_ports.size() > 1024 ||
            std::find(external_ports.begin(), external_ports.end(), output_port) ==
                external_ports.end()) {
            throw std::invalid_argument("invalid coherent pipeline network ports/builder");
        }
        auto work = begin(*state_, true);
        std::vector<PortCoherentComponent> incident;
        std::map<std::uint64_t, StreamRecord> lookup;
        for (const auto &input : inputs) {
            const auto &snapshot = inspect(input.stream);
            if (std::find(external_ports.begin(), external_ports.end(), input.input_port) ==
                external_ports.end()) {
                throw std::invalid_argument("input must be a selected external port");
            }
            auto flat = flatten(*work, snapshot);
            if (flat.components.size() > 4096 - incident.size()) {
                throw std::length_error("coherent pipeline expanded network limit exceeded");
            }
            for (const auto &component : flat.components) {
                incident.push_back({input.input_port, component});
            }
            lookup.insert(std::make_move_iterator(flat.lookup.begin()),
                          std::make_move_iterator(flat.lookup.end()));
        }
        const auto transmitted = transmit_coherent_network(
            spacing_, incident, external_ports, output_port, reference_, build);
        std::vector<StreamRecord> output;
        for (const auto &component : transmitted.components) {
            auto record = lookup.at(component.coherence_group);
            record.history[0].amplitude = component.amplitude;
            output.push_back(std::move(record));
        }
        return store(std::move(work), std::move(output));
    }

    PolynomialResult polynomial(Stream input,
                                const std::vector<double> &coefficients,
                                std::size_t max_source_order) {
        const Operation operation(active_);
        const auto &snapshot = inspect(input);
        if (max_source_order == 0 || max_source_order > 256) {
            throw std::invalid_argument("coherent pipeline source order must be in 1..256");
        }
        const CoherentPolynomial model(coefficients, reference_);
        auto work = begin(*state_, true);
        const auto flat = flatten(*work, snapshot);
        const auto response = model.evaluate(spacing_, flat.components, work->highest_group);
        PolynomialCounts counts{
            snapshot.records.size(), flat.components.size(), response.terms.size(), 0, {}};
        std::vector<const StreamRecord *> parents;
        for (const auto &component : response.inputs) {
            parents.push_back(&flat.lookup.at(component.coherence_group));
        }
        std::vector<StreamRecord> output;
        for (const auto &term : response.terms) {
            std::vector<int> indices(term.input_indices.begin(),
                                     term.input_indices.begin() + term.order);
            std::size_t source_order = 0;
            for (int index : indices) {
                source_order += parents[static_cast<std::size_t>(std::abs(index) - 1)]
                                    ->history[0]
                                    .factors.size();
            }
            if (source_order > max_source_order) {
                ++counts.discarded_term_count;
                ++counts.discarded_by_source_order[source_order];
                continue;
            }
            auto physical = compose(parents, indices, true);
            auto history = compose(parents, indices, false);
            auto component = term.component;
            component.coherence_group = 0;
            if (term.order > 1) {
                const bool harmonic =
                    std::all_of(physical.begin(), physical.end(), [&](const auto &factor) {
                        return factor.sign == 1 && factor.root_id == physical.front().root_id;
                    });
                component.kind = harmonic ? SpectrumKind::harmonic : SpectrumKind::intermod;
            }
            output.push_back(
                {component, {std::move(physical)}, {{std::move(history), component.amplitude}}});
        }
        return {store(std::move(work), std::move(output)), std::move(counts)};
    }

    CompressionResult compress_fundamentals(Stream input,
                                            const SaturatingFundamentalCompression &model) {
        const Operation operation(active_);
        const auto &snapshot = inspect(input);
        for (const auto &record : snapshot.records) {
            if (record.component.kind != SpectrumKind::source) {
                throw std::invalid_argument("coherent pipeline compression requires source kind");
            }
        }
        const auto drive = snapshot.reduction.total_power_w;
        const auto gain = model.amplitude_gain(drive);
        auto work = begin(*state_, true);
        auto output = snapshot.records;
        for (auto &record : output) {
            record.history = product_origin_expressions({record.history}, {1}, {gain, 0.});
        }
        return {store(std::move(work), std::move(output)), drive, gain};
    }
};
} // namespace rfmodel
