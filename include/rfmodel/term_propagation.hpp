#pragma once
#include "multitone_amplifier.hpp"
#include "spectrum_analysis.hpp"
#include <set>

namespace rfmodel {
// Preserve local mixing identity through a matched linear two-port reduction.
// Input/output order and zero-amplitude terms are preserved, including notches.
inline std::vector<AmplifierMixingTerm>
transmit_linear_terms(double spacing_hz,
                      const std::vector<AmplifierMixingTerm> &terms,
                      const std::vector<std::size_t> &external_ports,
                      double reference_ohms,
                      const std::function<LinearNetwork(double)> &build) {
    if (terms.size() > 4096 || !std::isfinite(spacing_hz) || spacing_hz <= 0 || !build ||
        external_ports.size() != 2 || !std::isfinite(reference_ohms) || reference_ohms <= 0) {
        throw std::invalid_argument("invalid linear term request");
    }
    std::set<std::tuple<int, int, std::array<int, 3>>> identities;
    PowerWaveSpectrum probe{spacing_hz, {}};
    for (const auto &term : terms) {
        if (term.order < 1 || term.order > 3 || term.bin <= 0 ||
            !std::isfinite(term.bin * spacing_hz) || !std::isfinite(std::norm(term.amplitude))) {
            throw std::invalid_argument("invalid RF mixing term");
        }
        long long sum = 0;
        for (int i = 0; i < 3; ++i) {
            if (i < term.order) {
                if (term.contributors[i] == 0 ||
                    term.contributors[i] == std::numeric_limits<int>::min() ||
                    (i > 0 && term.contributors[i] < term.contributors[i - 1])) {
                    throw std::invalid_argument("invalid signed mixing contributors");
                }
                sum += term.contributors[i];
            } else if (term.contributors[i] != 0) {
                throw std::invalid_argument("mixing contributor padding must be zero");
            }
        }
        if (sum != term.bin ||
            !identities.emplace(term.order, term.bin, term.contributors).second) {
            throw std::invalid_argument("inconsistent or duplicate mixing identity");
        }
        // Unit probes are independent of cancellation among physical terms.
        probe.amplitudes[term.bin] = 1.;
    }
    if (terms.empty()) {
        const auto network = build(0.);
        if (network.reference_impedance_ohms() != reference_ohms) {
            throw std::invalid_argument("linear term reference differs");
        }
        network.external_s(external_ports);
    }
    const auto transfer = transmit_linear_spectrum(probe, external_ports, reference_ohms, build);
    auto output = terms;
    for (auto &term : output) {
        const auto found = transfer.amplitudes.find(term.bin);
        term.amplitude *= found == transfer.amplitudes.end() ? Complex{} : found->second;
        if (!std::isfinite(std::norm(term.amplitude))) {
            throw std::overflow_error("linear term output power overflow");
        }
    }
    return output;
}
} // namespace rfmodel
