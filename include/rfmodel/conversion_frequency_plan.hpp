#pragma once
#include "conversion_operating_point.hpp"
#include "nonlinear_spectrum.hpp"
#include <set>
#include <string>
#include <type_traits>
#include <variant>

namespace rfmodel {
// Rules describe structural support, including channels whose current wave is zero.
struct FrequencyCopyRule {
    std::size_t input_port, output_port;
};

struct FrequencyPolynomialRule {
    std::size_t input_port, output_port;
    MemorylessPolynomial polynomial;
};

struct FrequencyMixerRule {
    std::size_t rf_port, lo_port, if_port;
};

// All RF inputs propagate directly; only the explicitly selected RF bins
// generate products. The polynomial is c0..c11 with c0=c1=0 (direct gain is
// separate), as returned by CoherentHighOrderAmplifier::voltage_coefficients.
struct FrequencyHighOrderRule {
    std::size_t input_port, output_port;
    MemorylessPolynomial polynomial;
    std::vector<int> generating_bins;
};

using ConversionFrequencyRule = std::
    variant<FrequencyCopyRule, FrequencyPolynomialRule, FrequencyMixerRule, FrequencyHighOrderRule>;

struct ConversionFrequencyDevice {
    std::vector<std::size_t> ports;
    std::vector<ConversionChannel> seeds;
    std::vector<ConversionFrequencyRule> rules;
};

struct ConversionFrequencyOptions {
    std::size_t max_total_channels = 512;
    int max_bin = std::numeric_limits<int>::max();
    std::size_t max_transitions = 10000000;
};

struct ConversionFrequencyPlan {
    // Device order is unchanged; each local grid is sorted by physical port, then bin.
    std::vector<std::vector<ConversionChannel>> channels;
    // Original seed order -> local channel index, for scattering source/noise data.
    std::vector<std::vector<std::size_t>> seed_channel_indices;
    std::size_t total_channels{}, generated_channels{}, iterations{}, transitions{};
};

// Dense linear N-port support, including reverse transmission. A star of copy
// rules generates the same union as all-to-all coupling. For known structural
// zeros callers may supply only the actual directed copy rules instead.
inline std::vector<ConversionFrequencyRule>
linear_frequency_rules(const std::vector<std::size_t> &ports) {
    if (ports.empty() || ports.size() > 512) {
        throw std::invalid_argument("linear frequency rule port count");
    }
    std::set<std::size_t> unique;
    for (auto port : ports) {
        if (port >= 1024 || !unique.insert(port).second) {
            throw std::invalid_argument("invalid or duplicate frequency port");
        }
    }
    std::vector<ConversionFrequencyRule> rules;
    for (std::size_t i = 1; i < ports.size(); ++i) {
        rules.emplace_back(FrequencyCopyRule{ports[0], ports[i]});
        rules.emplace_back(FrequencyCopyRule{ports[i], ports[0]});
    }
    return rules;
}

// Least finite closure of the declared rules and physical wires. Limits are
// failure bounds, never spectral truncation. No partial grid escapes on failure.
inline ConversionFrequencyPlan
plan_conversion_frequencies(double spacing_hz,
                            const std::vector<ConversionFrequencyDevice> &devices,
                            const std::vector<ConversionPortConnection> &connections,
                            const ConversionFrequencyOptions &options = {}) {
    if (!std::isfinite(spacing_hz) || spacing_hz <= 0. || devices.empty() || devices.size() > 512 ||
        options.max_total_channels == 0 || options.max_total_channels > 512 ||
        options.max_bin < 0 || options.max_transitions == 0 || options.max_transitions > 10000000) {
        throw std::invalid_argument("invalid conversion frequency plan options");
    }
    using PortBins = std::map<std::size_t, std::set<int>>;
    std::vector<PortBins> grids(devices.size());
    std::size_t port_count = 0, rule_count = 0;
    for (std::size_t d = 0; d < devices.size(); ++d) {
        const auto &device = devices[d];
        if (device.ports.empty() || device.ports.size() > 512 || device.rules.size() > 4096 ||
            device.seeds.size() > 512) {
            throw std::invalid_argument("invalid frequency device dimensions");
        }
        for (auto port : device.ports) {
            if (port >= 1024 || !grids[d].emplace(port, std::set<int>{}).second) {
                throw std::invalid_argument("invalid or duplicate frequency port");
            }
        }
        port_count += device.ports.size();
        rule_count += device.rules.size();
        if (port_count > options.max_total_channels || rule_count > 4096) {
            throw std::length_error("frequency plan declaration limit exceeded");
        }
        const auto known = [&](std::size_t port) {
            if (!grids[d].count(port)) {
                throw std::invalid_argument("frequency rule refers to undeclared port");
            }
        };
        for (const auto &rule : device.rules) {
            std::visit(
                [&](const auto &value) {
                    using Rule = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Rule, FrequencyMixerRule>) {
                        known(value.rf_port);
                        known(value.lo_port);
                        known(value.if_port);
                        if (value.rf_port == value.lo_port || value.rf_port == value.if_port ||
                            value.lo_port == value.if_port) {
                            throw std::invalid_argument("frequency mixer ports must differ");
                        }
                    } else {
                        known(value.input_port);
                        known(value.output_port);
                        if (value.input_port == value.output_port) {
                            throw std::invalid_argument(
                                "frequency rule input/output ports must differ");
                        }
                        if constexpr (std::is_same_v<Rule, FrequencyHighOrderRule>) {
                            if (value.polynomial.coefficient(0) != 0. ||
                                value.polynomial.coefficient(1) != 0. ||
                                value.generating_bins.size() > 512) {
                                throw std::invalid_argument("invalid high-order frequency rule");
                            }
                            std::set<int> selected;
                            for (auto bin : value.generating_bins) {
                                if (bin <= 0 || !selected.insert(bin).second) {
                                    throw std::invalid_argument(
                                        "invalid or duplicate high-order generating bin");
                                }
                            }
                        }
                    }
                },
                rule);
        }
    }
    if (connections.size() > port_count / 2) {
        throw std::invalid_argument("too many frequency plan connections");
    }
    std::set<std::pair<std::size_t, std::size_t>> connected;
    for (const auto &wire : connections) {
        for (auto endpoint : {std::make_pair(wire.first_device, wire.first_port),
                              std::make_pair(wire.second_device, wire.second_port)}) {
            if (endpoint.first >= devices.size() || !grids[endpoint.first].count(endpoint.second) ||
                !connected.insert(endpoint).second) {
                throw std::invalid_argument("unknown or multiply connected frequency port");
            }
        }
    }
    ConversionFrequencyPlan result;
    auto charge = [&] {
        if (result.transitions >= options.max_transitions) {
            throw std::length_error("frequency plan transition limit exceeded");
        }
        ++result.transitions;
    };
    auto insert = [&](std::size_t device, std::size_t port, long long bin) {
        charge();
        if (bin < 0 || bin > std::numeric_limits<int>::max() ||
            !std::isfinite(static_cast<double>(bin) * spacing_hz)) {
            throw std::overflow_error("frequency plan frequency is not representable");
        }
        if (bin > options.max_bin) {
            throw std::length_error("frequency plan maximum bin exceeded at device " +
                                    std::to_string(device) + " port " + std::to_string(port) +
                                    " bin " + std::to_string(bin));
        }
        auto &bins = grids[device].at(port);
        if (bins.count(static_cast<int>(bin))) {
            return false;
        }
        if (result.total_channels == options.max_total_channels) {
            throw std::length_error("frequency plan total channel limit exceeded");
        }
        bins.insert(static_cast<int>(bin));
        ++result.total_channels;
        return true;
    };
    std::size_t seed_count = 0;
    for (std::size_t d = 0; d < devices.size(); ++d) {
        std::set<std::pair<std::size_t, int>> unique;
        for (auto seed : devices[d].seeds) {
            if (!grids[d].count(seed.port) || seed.bin < 0 ||
                !unique.emplace(seed.port, seed.bin).second) {
                throw std::invalid_argument(
                    "unknown port, negative bin or duplicate frequency seed");
            }
            insert(d, seed.port, seed.bin);
            ++seed_count;
        }
    }

    struct CachedInputs {
        bool evaluated = false;
        std::set<int> first, second;
    };

    std::vector<std::vector<CachedInputs>> cached(devices.size());
    for (std::size_t d = 0; d < devices.size(); ++d) {
        cached[d].resize(devices[d].rules.size());
    }
    bool changed;
    do {
        changed = false;
        ++result.iterations;
        for (std::size_t d = 0; d < devices.size(); ++d) {
            for (std::size_t r = 0; r < devices[d].rules.size(); ++r) {
                std::visit(
                    [&](const auto &rule) {
                        using Rule = std::decay_t<decltype(rule)>;
                        std::set<int> first, second;
                        if constexpr (std::is_same_v<Rule, FrequencyMixerRule>) {
                            first = grids[d].at(rule.rf_port);
                            second = grids[d].at(rule.lo_port);
                        } else {
                            first = grids[d].at(rule.input_port);
                        }
                        auto &previous = cached[d][r];
                        if (previous.evaluated && first == previous.first &&
                            second == previous.second) {
                            return;
                        }
                        previous = {true, first, second};
                        if constexpr (std::is_same_v<Rule, FrequencyCopyRule>) {
                            for (auto bin : first) {
                                changed = insert(d, rule.output_port, bin) || changed;
                            }
                        } else if constexpr (std::is_same_v<Rule, FrequencyMixerRule>) {
                            for (long long rf : first) {
                                for (long long lo : second) {
                                    changed = insert(d, rule.if_port, rf + lo) || changed;
                                    changed = insert(d, rule.if_port, std::abs(rf - lo)) || changed;
                                }
                            }
                        } else {
                            if constexpr (std::is_same_v<Rule, FrequencyHighOrderRule>) {
                                for (auto bin : first) {
                                    if (bin > 0) {
                                        changed = insert(d, rule.output_port, bin) || changed;
                                    }
                                }
                                // Selected bins may arrive after wire propagation.
                                // Check their presence only after closure.
                                std::set<int> selected;
                                for (auto bin : rule.generating_bins) {
                                    if (first.count(bin)) {
                                        selected.insert(bin);
                                    }
                                }
                                first = std::move(selected);
                            }
                            if (rule.polynomial.coefficient(0) != 0.) {
                                changed = insert(d, rule.output_port, 0) || changed;
                            }
                            if (first.empty()) {
                                return;
                            }
                            std::size_t degree = maximum_polynomial_order;
                            while (degree > 0 && rule.polynomial.coefficient(degree) == 0.) {
                                --degree;
                            }
                            std::set<long long> signed_bins;
                            for (long long bin : first) {
                                signed_bins.insert(bin);
                                signed_bins.insert(-bin);
                            }
                            std::set<long long> support{0};
                            std::set<long long> highorder_outputs;
                            for (std::size_t order = 1; order <= degree; ++order) {
                                std::set<long long> next;
                                for (auto left : support) {
                                    for (auto right : signed_bins) {
                                        charge();
                                        next.insert(left + right);
                                        if (next.size() > 4096) {
                                            throw std::length_error(
                                                "frequency polynomial support limit exceeded");
                                        }
                                    }
                                }
                                support = std::move(next);
                                if (rule.polynomial.coefficient(order) != 0.) {
                                    for (auto bin : support) {
                                        if (bin >= 0) {
                                            if constexpr (std::is_same_v<Rule,
                                                                         FrequencyHighOrderRule>) {
                                                highorder_outputs.insert(bin);
                                                // DC counts in the linearizer's internal
                                                // polynomial before RF projection.
                                                if (first.size() + highorder_outputs.size() > 512) {
                                                    throw std::length_error(
                                                        "high-order internal polynomial channel "
                                                        "limit exceeded");
                                                }
                                                if (bin == 0) {
                                                    continue;
                                                }
                                            }
                                            changed = insert(d, rule.output_port, bin) || changed;
                                        }
                                    }
                                }
                            }
                        }
                    },
                    devices[d].rules[r]);
            }
        }
        for (const auto &wire : connections) {
            const auto &first = grids[wire.first_device].at(wire.first_port);
            const auto &second = grids[wire.second_device].at(wire.second_port);
            for (auto bin : first) {
                changed = insert(wire.second_device, wire.second_port, bin) || changed;
            }
            for (auto bin : second) {
                changed = insert(wire.first_device, wire.first_port, bin) || changed;
            }
        }
    } while (changed);
    result.channels.resize(devices.size());
    result.seed_channel_indices.resize(devices.size());
    for (std::size_t d = 0; d < devices.size(); ++d) {
        for (const auto &rule : devices[d].rules) {
            if (const auto *highorder = std::get_if<FrequencyHighOrderRule>(&rule)) {
                const auto &inputs = grids[d].at(highorder->input_port);
                if (inputs.empty() || *inputs.rbegin() == 0) {
                    throw std::invalid_argument("high-order frequency rule requires RF input");
                }
                for (auto bin : highorder->generating_bins) {
                    if (!inputs.count(bin)) {
                        throw std::invalid_argument("missing high-order generating input bin");
                    }
                }
            }
        }
        std::map<std::pair<std::size_t, int>, std::size_t> indices;
        for (const auto &port : grids[d]) {
            if (port.second.empty()) {
                throw std::invalid_argument(
                    "frequency plan leaves an unseeded port without channels");
            }
            for (auto bin : port.second) {
                indices.emplace(std::make_pair(port.first, bin), result.channels[d].size());
                result.channels[d].push_back({port.first, bin});
            }
        }
        for (auto seed : devices[d].seeds) {
            result.seed_channel_indices[d].push_back(indices.at({seed.port, seed.bin}));
        }
    }
    result.generated_channels = result.total_channels - seed_count;
    return result;
}
} // namespace rfmodel
