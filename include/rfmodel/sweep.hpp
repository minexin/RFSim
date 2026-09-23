#pragma once
#include "network.hpp"
#include <functional>

namespace rfmodel {
struct FrequencySolution {
    double frequency_hz;
    NetworkWaves waves;
};

// Builder is invoked only after the entire grid passes validation.
inline std::vector<FrequencySolution>
sweep_network(const FrequencyGrid &grid, const std::function<LinearNetwork(double)> &build) {
    if (grid.hz.empty() || !build) {
        throw std::invalid_argument("empty sweep or builder");
    }
    for (std::size_t i = 0; i < grid.hz.size(); ++i) {
        if (!std::isfinite(grid.hz[i]) || grid.hz[i] < 0 || (i && grid.hz[i] <= grid.hz[i - 1])) {
            throw std::invalid_argument("sweep frequencies must be finite and strictly increasing");
        }
    }
    std::vector<FrequencySolution> result;
    result.reserve(grid.hz.size());
    for (const double f : grid.hz) {
        try {
            result.push_back({f, build(f).solve()});
        } catch (const std::exception &error) {
            throw std::runtime_error("network sweep failed at " + std::to_string(f) +
                                     " Hz: " + error.what());
        }
    }
    return result;
}
} // namespace rfmodel
