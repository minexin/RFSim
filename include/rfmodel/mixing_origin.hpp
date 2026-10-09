#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace rfmodel {
struct OriginFactor {
    std::uint64_t root_id{};
    int sign{1};

    bool operator==(const OriginFactor &other) const noexcept {
        return root_id == other.root_id && sign == other.sign;
    }
};

using MixingOrigin = std::vector<OriginFactor>;

// Compose a product of parent monomials, conjugating every factor for a negative
// one-based parent index. Repeated roots and opposite signs never cancel.
// Parent order need not be canonical; the result is sorted by (root_id, sign).
inline MixingOrigin expand_mixing_origin(const std::vector<MixingOrigin> &parents,
                                         const std::vector<int> &indices) {
    if (parents.size() > 4096 || indices.empty() || indices.size() > 9) {
        throw std::invalid_argument("invalid mixing-origin parent/product count");
    }
    std::size_t stored = 0;
    for (const auto &parent : parents) {
        if (parent.empty() || parent.size() > 256 || (stored += parent.size()) > 65536) {
            throw std::invalid_argument("invalid mixing-origin factor count");
        }
        for (const auto &factor : parent) {
            if (factor.root_id == 0 || (factor.sign != 1 && factor.sign != -1)) {
                throw std::invalid_argument("invalid mixing-origin factor");
            }
        }
    }
    MixingOrigin result;
    for (int index : indices) {
        const auto absolute = std::abs(static_cast<long long>(index));
        if (absolute == 0 || static_cast<std::size_t>(absolute) > parents.size()) {
            throw std::invalid_argument("mixing-origin index outside parent array");
        }
        const auto &parent = parents[static_cast<std::size_t>(absolute - 1)];
        if (result.size() + parent.size() > 256) {
            throw std::length_error("expanded mixing origin exceeds 256 factors");
        }
        for (const auto &factor : parent) {
            result.push_back({factor.root_id, (index < 0 ? -1 : 1) * factor.sign});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return std::tie(left.root_id, left.sign) < std::tie(right.root_id, right.sign);
    });
    return result;
}
} // namespace rfmodel
