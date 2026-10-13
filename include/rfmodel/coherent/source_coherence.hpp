#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rfmodel {
struct SourceCoherence {
    std::string source_id;
    std::string reference_clock;
};

// Resolve source relationships only. Frequency, kind and bandwidth remain
// independent coherence constraints. IDs are local to this complete source set.
inline std::vector<std::uint64_t>
assign_source_coherence(const std::vector<SourceCoherence> &sources) {
    if (sources.size() > 4096) {
        throw std::invalid_argument("at most 4096 source definitions");
    }
    using Key = std::pair<bool, std::string>;
    std::set<std::string> source_ids;
    std::map<Key, std::uint64_t> groups;
    auto key = [](const SourceCoherence &source) {
        const bool locked = !source.reference_clock.empty();
        return Key{locked, locked ? source.reference_clock : source.source_id};
    };
    for (const auto &source : sources) {
        if (source.source_id.empty() || source.source_id.size() > 1024 ||
            source.reference_clock.size() > 1024 ||
            source.source_id.find('\0') != std::string::npos ||
            source.reference_clock.find('\0') != std::string::npos) {
            throw std::invalid_argument("invalid source or reference clock label");
        }
        if (!source_ids.emplace(source.source_id).second) {
            throw std::invalid_argument("duplicate source definition");
        }
        groups.emplace(key(source), 0);
    }
    std::uint64_t next = 1;
    for (auto &entry : groups) {
        entry.second = next++;
    }
    std::vector<std::uint64_t> result;
    result.reserve(sources.size());
    for (const auto &source : sources) {
        result.push_back(groups.at(key(source)));
    }
    return result;
}
} // namespace rfmodel
