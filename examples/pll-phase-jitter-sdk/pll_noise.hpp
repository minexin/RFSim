#pragma once
#include <rfmodel/noise/locked_pi_pll.hpp>
#include <rfmodel/noise/phase_jitter.hpp>
#include <array>
#include <vector>

namespace example {
struct OffsetResult {
    double offset_hz{};
    std::array<rfmodel::LockedPiPllResult, 2> pll;
    rfmodel::ConversionNoise incident_noise; // Complete C/P, W/Hz, six channels.
    rfmodel::Complex cross_phase_rad2_per_hz{};
    double differential_common_rad2_per_hz{}; // Example oracle only, not a jitter API.
    double differential_vco_rad2_per_hz{};
};

struct Result {
    double carrier_hz{};
    std::vector<OffsetResult> offsets;
    std::array<rfmodel::PhaseJitterMeasurement, 2> jitter;
};

// Caller supplies strictly increasing positive integer-Hz offsets. Integer Hz
// represent the conversion grid exactly; logarithmic spacing is also supported.
Result simulate(const std::vector<int> &offsets_hz, bool equal_loops);
} // namespace example
