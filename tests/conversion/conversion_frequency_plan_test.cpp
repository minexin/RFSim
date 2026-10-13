#include "rfmodel/conversion/conversion_frequency_plan.hpp"
#include "rfmodel/conversion/mixer_linearization.hpp"
#include "rfmodel/conversion/polynomial_linearization.hpp"
#include "../support/test_support.hpp"
#include <algorithm>
#include <iostream>

using namespace rfmodel;

std::vector<int> bins(const ConversionFrequencyPlan &plan, std::size_t device, std::size_t port) {
    std::vector<int> result;
    for (auto channel : plan.channels.at(device)) {
        if (channel.port == port) {
            result.push_back(channel.bin);
        }
    }
    return result;
}

std::size_t index(const std::vector<ConversionChannel> &channels, std::size_t port, int bin) {
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].port == port && channels[i].bin == bin) {
            return i;
        }
    }
    throw std::runtime_error("missing test channel");
}

bool same_channels(const ConversionFrequencyPlan &a, const ConversionFrequencyPlan &b) {
    if (a.channels.size() != b.channels.size()) {
        return false;
    }
    for (std::size_t d = 0; d < a.channels.size(); ++d) {
        if (a.channels[d].size() != b.channels[d].size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.channels[d].size(); ++i) {
            if (a.channels[d][i].port != b.channels[d][i].port ||
                a.channels[d][i].bin != b.channels[d][i].bin) {
                return false;
            }
        }
    }
    return true;
}

std::vector<ConversionFrequencyDevice> chain_description() {
    return {{{1, 0}, {{0, 3}, {0, 2}}, linear_frequency_rules({0, 1})},
            {{2, 0, 1}, {{1, 1}}, {FrequencyMixerRule{0, 1, 2}}},
            {{0, 1}, {}, {FrequencyPolynomialRule{0, 1, MemorylessPolynomial({.2, 1., .1})}}}};
}

void structural_closure() {
    auto description = chain_description();
    std::vector<ConversionPortConnection> wires{{0, 1, 1, 0}, {1, 2, 2, 0}};
    const auto plan = plan_conversion_frequencies(1e6, description, wires);
    require(plan.total_channels == 24 && plan.generated_channels == 21,
            "wrong chain channel count");
    require(bins(plan, 0, 0) == std::vector<int>({2, 3}) &&
                bins(plan, 1, 0) == std::vector<int>({2, 3}),
            "RF copy/wire closure differs");
    require(bins(plan, 1, 2) == std::vector<int>({1, 2, 3, 4}) &&
                bins(plan, 2, 0) == std::vector<int>({1, 2, 3, 4}),
            "mixer/wire closure differs");
    require(bins(plan, 2, 1) == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8}),
            "quadratic difference/sum closure differs");
    require(plan.seed_channel_indices[0] == std::vector<std::size_t>({1, 0}),
            "seed permutation lost");
    for (auto &device : description) {
        std::reverse(device.ports.begin(), device.ports.end());
        std::reverse(device.rules.begin(), device.rules.end());
    }
    std::reverse(wires.begin(), wires.end());
    require(same_channels(plan, plan_conversion_frequencies(1e6, description, wires)),
            "closure depends on declaration order");
    for (std::size_t d = 0; d < description.size(); ++d) {
        description[d].seeds = plan.channels[d];
    }
    const auto repeated = plan_conversion_frequencies(1e6, description, wires);
    require(same_channels(plan, repeated) && repeated.generated_channels == 0,
            "full closure is not idempotent");
    // An observation/noise-only RF channel must generate products, even if its
    // nominal amplitude will later be zero. Seed descriptors contain no amplitude.
    description = chain_description();
    description[0].seeds.push_back({0, 5});
    const auto extra = plan_conversion_frequencies(1e6, description, wires);
    require(bins(extra, 1, 2) == std::vector<int>({1, 2, 3, 4, 6}),
            "zero nominal sideband support was lost");
}

void linear_port_directions() {
    const auto dense = plan_conversion_frequencies(
        1., {{{7, 2, 9}, {{7, 1}, {9, 4}}, linear_frequency_rules({7, 2, 9})}}, {});
    for (auto port : {2u, 7u, 9u}) {
        require(bins(dense, 0, port) == std::vector<int>({1, 4}),
                "linear reverse/forward union differs");
    }
    require(dense.channels[0][0].port == 2 && dense.channels[0][2].port == 7,
            "noncontiguous physical ports are not sorted");
    const auto directed = plan_conversion_frequencies(
        1., {{{0, 1}, {{0, 1}, {1, 4}}, {FrequencyCopyRule{0, 1}}}}, {});
    require(bins(directed, 0, 0) == std::vector<int>({1}) &&
                bins(directed, 0, 1) == std::vector<int>({1, 4}),
            "directed structural zeros were ignored");
}

void dc_and_feedback() {
    const auto dc = plan_conversion_frequencies(
        1.,
        {{{0, 1}, {{0, 0}}, {FrequencyPolynomialRule{0, 1, MemorylessPolynomial({0., 0., 1.})}}}},
        {{0, 0, 0, 1}});
    require(dc.total_channels == 2 && bins(dc, 0, 1) == std::vector<int>({0}),
            "finite DC feedback did not close");
    const auto mixer = plan_conversion_frequencies(
        1., {{{0, 1, 2}, {{0, 0}, {0, 3}, {1, 0}, {1, 3}}, {FrequencyMixerRule{0, 1, 2}}}}, {});
    require(bins(mixer, 0, 2) == std::vector<int>({0, 3, 6}),
            "DC, equal-frequency difference or duplicate products differ");
    const auto loop = plan_conversion_frequencies(
        1., {{{0, 1, 2}, {{0, 4}, {1, 0}}, {FrequencyMixerRule{0, 1, 2}}}}, {{0, 0, 0, 2}});
    require(loop.total_channels == 3 && bins(loop, 0, 2) == std::vector<int>({4}),
            "zero-frequency LO feedback should have finite support");
    ConversionFrequencyOptions bounds;
    bounds.max_bin = 12;
    rejects<std::length_error>([&] {
        plan_conversion_frequencies(
            1.,
            {{{0, 1},
              {{0, 1}},
              {FrequencyPolynomialRule{0, 1, MemorylessPolynomial({0., 0., 1.})}}}},
            {{0, 0, 0, 1}},
            bounds);
    });
    rejects<std::length_error>([&] {
        plan_conversion_frequencies(1.,
                                    {{{0, 1, 2}, {{0, 4}, {1, 1}}, {FrequencyMixerRule{0, 1, 2}}}},
                                    {{0, 0, 0, 2}},
                                    bounds);
    });
    const auto constant = plan_conversion_frequencies(
        1., {{{0, 1}, {{0, 7}}, {FrequencyPolynomialRule{0, 1, MemorylessPolynomial({2.})}}}}, {});
    require(bins(constant, 0, 1) == std::vector<int>({0}), "constant polynomial must generate DC");
    const auto zero = plan_conversion_frequencies(
        1.,
        {{{0, 1}, {{0, 7}, {1, 9}}, {FrequencyPolynomialRule{0, 1, MemorylessPolynomial({0.})}}}},
        {});
    require(bins(zero, 0, 1) == std::vector<int>({9}), "zero polynomial generated false products");
    std::vector<double> coefficients(12);
    coefficients[11] = 1.;
    const auto odd = plan_conversion_frequencies(
        1.,
        {{{0, 1}, {{0, 2}}, {FrequencyPolynomialRule{0, 1, MemorylessPolynomial(coefficients)}}}},
        {});
    require(bins(odd, 0, 1) == std::vector<int>({2, 6, 10, 14, 18, 22}),
            "eleventh-order parity or harmonics differ");
}

void planned_network_solution() {
    const auto description = chain_description();
    const std::vector<ConversionPortConnection> wires{{0, 1, 1, 0}, {1, 2, 2, 0}};
    const auto plan = plan_conversion_frequencies(1., description, wires);
    std::vector<ConversionDevice> devices;
    for (std::size_t d = 0; d < plan.channels.size(); ++d) {
        const auto &channels = plan.channels[d];
        const auto n = channels.size();
        auto direct = conversion_detail::zero(n);
        if (d == 0) {
            for (int bin : {2, 3}) {
                direct(index(channels, 1, bin), index(channels, 0, bin)) = .8;
                direct(index(channels, 0, bin), index(channels, 1, bin)) = .8;
            }
        }
        FrequencyConversionModel model(1., channels, direct, conversion_detail::zero(n));
        devices.push_back({model,
                           std::vector<Complex>(n),
                           std::vector<Complex>(n),
                           model.zero_noise(),
                           model.zero_noise()});
    }
    const Complex rf2{.01, .002}, rf3{.02, -.003}, lo{.1, .02};
    devices[0].source[plan.seed_channel_indices[0][0]] = rf3;
    devices[0].source[plan.seed_channel_indices[0][1]] = rf2;
    devices[1].source[plan.seed_channel_indices[1][0]] = lo;
    const auto mixed_channels = plan.channels[1], polynomial_channels = plan.channels[2];
    std::vector<ConversionNonlinearDevice> nonlinear{
        {1,
         [mixed_channels](const std::vector<Complex> &a) {
             const auto point = linearize_bilinear_mixer(1., mixed_channels, a, 0., 1.);
             return ConversionLinearization{point.incremental_model, point.operating_outgoing};
         }},
        {2, [polynomial_channels](const std::vector<Complex> &a) {
             return linearize_polynomial_amplifier(
                 1., polynomial_channels, a, MemorylessPolynomial({.2, 1., .1}));
         }}};
    const auto solved = solve_conversion_operating_point(devices, wires, nonlinear);
    require(solved.scaled_residual <= 1., "planned network did not converge");
    // Independent time-domain multiplication, polynomial evaluation and Fourier
    // projection. Check every generated output; do not call support helpers here.
    const std::size_t offset = plan.channels[0].size() + plan.channels[1].size();
    constexpr int samples = 256;
    const double pi = std::acos(-1.), root_two = std::sqrt(2.), root_r = std::sqrt(50.);
    for (int bin = 0; bin <= 8; ++bin) {
        Complex projection{};
        for (int sample = 0; sample < samples; ++sample) {
            const double angle = 2. * pi * sample / samples;
            const double rf_wave = root_two * (.8 * rf2 * std::polar(1., 2. * angle) +
                                               .8 * rf3 * std::polar(1., 3. * angle))
                                                  .real();
            const double lo_wave = root_two * (lo * std::polar(1., angle)).real();
            const double voltage = root_r * root_two * rf_wave * lo_wave;
            const double output_wave = (.2 + voltage + .1 * voltage * voltage) / root_r;
            projection += output_wave * std::polar(1., -bin * angle) / static_cast<double>(samples);
        }
        if (bin != 0) {
            projection *= root_two;
        }
        near(solved.waves.outgoing[offset + index(polynomial_channels, 1, bin)], projection);
    }
    // Every physical wire was given equal frequency sets and solved consistently.
    for (auto wire : wires) {
        require(bins(plan, wire.first_device, wire.first_port) ==
                    bins(plan, wire.second_device, wire.second_port),
                "wire grids differ");
    }
}

void invalid_and_limits() {
    const ConversionFrequencyDevice wire{{0, 1}, {{0, 1}}, {FrequencyCopyRule{0, 1}}};
    rejects<std::invalid_argument>([&] {
        plan_conversion_frequencies(0., {wire}, {});
    });
    rejects<std::invalid_argument>([&] {
        plan_conversion_frequencies(1., {}, {});
    });
    rejects<std::invalid_argument>([&] {
        auto invalid = wire;
        invalid.ports.push_back(0);
        plan_conversion_frequencies(1., {invalid}, {});
    });
    rejects<std::invalid_argument>([&] {
        auto invalid = wire;
        invalid.seeds.push_back({0, 1});
        plan_conversion_frequencies(1., {invalid}, {});
    });
    rejects<std::invalid_argument>([&] {
        auto invalid = wire;
        invalid.seeds[0].bin = -1;
        plan_conversion_frequencies(1., {invalid}, {});
    });
    rejects<std::invalid_argument>([&] {
        auto invalid = wire;
        invalid.rules = {FrequencyCopyRule{0, 2}};
        plan_conversion_frequencies(1., {invalid}, {});
    });
    rejects<std::invalid_argument>([&] {
        auto invalid = wire;
        invalid.rules = {FrequencyMixerRule{0, 0, 1}};
        plan_conversion_frequencies(1., {invalid}, {});
    });
    rejects<std::invalid_argument>([&] {
        auto invalid = wire;
        invalid.seeds.clear();
        plan_conversion_frequencies(1., {invalid}, {});
    });
    rejects<std::invalid_argument>([&] {
        plan_conversion_frequencies(1., {wire, wire}, {{0, 0, 0, 0}});
    });
    rejects<std::invalid_argument>([&] {
        plan_conversion_frequencies(1., {wire, wire}, {{0, 1, 1, 0}, {0, 1, 1, 1}});
    });
    rejects<std::invalid_argument>([&] {
        plan_conversion_frequencies(1., {wire, wire}, {{0, 1, 2, 0}});
    });
    ConversionFrequencyOptions options;
    options.max_bin = 0;
    rejects<std::length_error>([&] {
        plan_conversion_frequencies(1., {wire}, {}, options);
    });
    options.max_bin = 10;
    options.max_transitions = 1;
    rejects<std::length_error>([&] {
        plan_conversion_frequencies(1., {wire}, {}, options);
    });
    options.max_transitions = 10000;
    options.max_total_channels = 3;
    rejects<std::length_error>([&] {
        auto large = wire;
        large.seeds.push_back({0, 2});
        plan_conversion_frequencies(1., {large}, {}, options);
    });
    options.max_total_channels = 2;
    const auto exact = plan_conversion_frequencies(1., {wire}, {}, options);
    require(exact.total_channels == 2, "exact channel limit should be permitted");
    rejects<std::overflow_error>([] {
        plan_conversion_frequencies(1.,
                                    {{{0, 1, 2},
                                      {{0, std::numeric_limits<int>::max()}, {1, 1}},
                                      {FrequencyMixerRule{0, 1, 2}}}},
                                    {});
    });
    rejects<std::overflow_error>([&] {
        auto large = wire;
        large.seeds[0].bin = 2;
        plan_conversion_frequencies(std::numeric_limits<double>::max(), {large}, {});
    });
    rejects<std::invalid_argument>([] {
        linear_frequency_rules({0, 0});
    });
}

int main() {
    try {
        structural_closure();
        linear_port_directions();
        dc_and_feedback();
        planned_network_solution();
        invalid_and_limits();
        std::cout << "conversion frequency planning tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
