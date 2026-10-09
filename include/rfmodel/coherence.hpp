#pragma once
#include "device_model.hpp"
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace rfmodel {
enum class SpectrumKind {
    source = 0,
    harmonic = 1,
    intermod = 2
};

// Deterministic RF voltage component in sqrt(W). Group IDs are explicit,
// local to this reduction context; different clocks require different IDs.
struct CoherentComponent {
    int bin{};
    SpectrumKind kind{};
    double bandwidth_hz{};
    std::uint64_t coherence_group{};
    Complex amplitude{};
};

struct CoherentReduction {
    std::vector<CoherentComponent> components;
    std::map<int, double> power_by_bin_w;
    double total_power_w{};
};

namespace coherence_detail {
struct Sum {
    double value{};
    double correction{};

    void add(double next) {
        const double updated = value + next;
        if (!std::isfinite(updated)) {
            throw std::overflow_error("coherent reduction sum overflow");
        }
        correction +=
            std::abs(value) >= std::abs(next) ? (value - updated) + next : (next - updated) + value;
        value = updated;
    }

    double result() const {
        const double total = value + correction;
        if (!std::isfinite(total)) {
            throw std::overflow_error("coherent reduction compensation overflow");
        }
        return total;
    }
};

struct WaveSum {
    Sum real;
    Sum imag;
};
} // namespace coherence_detail

// Equal (bin, kind, bandwidth, group) keys add coherently. Distinct keys add
// power, even at the same frequency. No clock/group inference or noise PSD.
inline CoherentReduction reduce_coherent_components(double spacing_hz,
                                                    const std::vector<CoherentComponent> &input) {
    if (!std::isfinite(spacing_hz) || spacing_hz <= 0 || input.size() > 4096) {
        throw std::invalid_argument("invalid coherent reduction grid or size");
    }
    using Key = std::tuple<int, SpectrumKind, double, std::uint64_t>;
    std::map<Key, coherence_detail::WaveSum> groups;
    for (const auto &component : input) {
        const double frequency = component.bin * spacing_hz;
        if (component.bin <= 0 || !std::isfinite(frequency) ||
            component.kind < SpectrumKind::source || component.kind > SpectrumKind::intermod ||
            !std::isfinite(component.bandwidth_hz) || component.bandwidth_hz <= 0 ||
            frequency - component.bandwidth_hz / 2 < 0 ||
            !std::isfinite(frequency + component.bandwidth_hz / 2) ||
            component.coherence_group == 0 || !std::isfinite(std::norm(component.amplitude))) {
            throw std::invalid_argument("invalid deterministic RF coherence component");
        }
        auto &sum = groups[{
            component.bin, component.kind, component.bandwidth_hz, component.coherence_group}];
        sum.real.add(component.amplitude.real());
        sum.imag.add(component.amplitude.imag());
    }
    CoherentReduction result;
    std::map<int, coherence_detail::Sum> powers;
    for (const auto &entry : groups) {
        const auto &key = entry.first;
        const Complex amplitude{entry.second.real.result(), entry.second.imag.result()};
        const double power = std::norm(amplitude);
        if (!std::isfinite(power)) {
            throw std::overflow_error("coherent group power overflow");
        }
        const int bin = std::get<0>(key);
        result.components.push_back(
            {bin, std::get<1>(key), std::get<2>(key), std::get<3>(key), amplitude});
        powers[bin].add(power);
    }
    coherence_detail::Sum total;
    for (const auto &entry : powers) {
        const double power = entry.second.result();
        result.power_by_bin_w.emplace(entry.first, power);
        total.add(power);
    }
    result.total_power_w = total.result();
    return result;
}
} // namespace rfmodel
