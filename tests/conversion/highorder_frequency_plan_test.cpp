#include "rfmodel/conversion/conversion_frequency_plan.hpp"
#include "rfmodel/conversion/highorder_linearization.hpp"
#include "../support/test_support.hpp"
#include <algorithm>
#include <iostream>

using namespace rfmodel;

std::vector<int> bins(const ConversionFrequencyPlan &plan, std::size_t d, std::size_t port) {
    std::vector<int> result;
    for (auto ch : plan.channels.at(d)) {
        if (ch.port == port) {
            result.push_back(ch.bin);
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

void selection_and_projection() {
    const MemorylessPolynomial polynomial({0., 0., .1, .2});
    // Selected bins arrive from another device after the first rule visit.
    std::vector<ConversionFrequencyDevice> devices{
        {{0, 1}, {}, {FrequencyHighOrderRule{0, 1, polynomial, {2, 1}}}},
        {{7}, {{7, 13}, {7, 2}, {7, 0}, {7, 1}}, {}}};
    const std::vector<ConversionPortConnection> wires{{0, 0, 1, 7}};
    const auto plan = plan_conversion_frequencies(1e6, devices, wires);
    require(bins(plan, 0, 0) == std::vector<int>({0, 1, 2, 13}), "incoming selection lost");
    require(bins(plan, 0, 1) == std::vector<int>({1, 2, 3, 4, 5, 6, 13}),
            "unselected spur generated products or DC was propagated");
    require(plan.total_channels == 15 && plan.generated_channels == 11,
            "wrong selected closure size");
    require(plan.seed_channel_indices[1] == std::vector<std::size_t>({3, 2, 0, 1}),
            "seed mapping lost");
    const CoherentHighOrderAmplifier amplifier(20., 20., 23., {.1, .2});
    std::vector<Complex> input(plan.channels[0].size());
    input[index(plan.channels[0], 0, 1)] = .01;
    input[index(plan.channels[0], 0, 2)] = {.003, .002};
    input[index(plan.channels[0], 0, 13)] = {.02, .01};
    const auto point =
        linearize_highorder_amplifier(1e6, plan.channels[0], input, amplifier, {2, 1});
    const auto third = index(plan.channels[0], 1, 3), spur = index(plan.channels[0], 0, 13);
    require(std::abs(point.jacobian.direct()(third, spur)) > 1e-8,
            "unselected spur must still affect products through common drive");
    // Explicit DC observation remains legal, but does not acquire a DC signal.
    devices[0].seeds = {{1, 0}};
    const auto dc = plan_conversion_frequencies(1e6, devices, wires);
    std::vector<Complex> dc_input(dc.channels[0].size());
    dc_input[index(dc.channels[0], 0, 1)] = .01;
    const auto dc_point =
        linearize_highorder_amplifier(1e6, dc.channels[0], dc_input, amplifier, {1, 2});
    near(dc_point.outgoing[index(dc.channels[0], 1, 0)], 0.);
    dc_input[index(dc.channels[0], 0, 0)] = .01;
    rejects<std::invalid_argument>([&] {
        linearize_highorder_amplifier(1e6, dc.channels[0], dc_input, amplifier, {1, 2});
    });
    // A complete plan remains unchanged when all channels are supplied as seeds.
    for (std::size_t d = 0; d < devices.size(); ++d) {
        devices[d].seeds = dc.channels[d];
    }
    const auto repeated = plan_conversion_frequencies(1e6, devices, wires);
    require(repeated.generated_channels == 0 && repeated.total_channels == dc.total_channels,
            "selected closure is not idempotent");
    std::swap(devices[0], devices[1]);
    const auto reordered = plan_conversion_frequencies(1e6, devices, {{1, 0, 0, 7}});
    require(bins(reordered, 1, 1) == bins(dc, 0, 1), "closure depends on device order");
}

void direct_and_eleventh_order() {
    for (auto rule : {FrequencyHighOrderRule{0, 1, MemorylessPolynomial({0.}), {2}},
                      FrequencyHighOrderRule{0, 1, MemorylessPolynomial({0., 0., 1.}), {}}}) {
        const auto plan =
            plan_conversion_frequencies(1., {{{0, 1}, {{0, 0}, {0, 2}, {0, 17}}, {rule}}}, {});
        require(bins(plan, 0, 1) == std::vector<int>({2, 17}), "direct-only RF support differs");
    }
    std::vector<double> coefficients(12);
    coefficients[11] = .1;
    const auto plan = plan_conversion_frequencies(
        1.,
        {{{0, 1},
          {{0, 2}, {0, 13}},
          {FrequencyHighOrderRule{0, 1, MemorylessPolynomial(coefficients), {2}}}}},
        {});
    require(bins(plan, 0, 1) == std::vector<int>({2, 6, 10, 13, 14, 18, 22}),
            "selected eleventh order support differs");
}

// Independent scalar gain/limiter construction for 20 dB, P1dB=20 dBm,
// Psat=23 dBm. The scalar fixed point includes harmonic feedback in total drive.
std::pair<double, double> gains(double power) {
    const double radius = std::sqrt(power), q = std::pow(10., -.05);
    const double p1 = std::sqrt(std::pow(10., -2.9));
    const double head = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1);
    const double g =
        radius <= p1
            ? 10 * (1 - (1 - q) * power / (p1 * p1))
            : (std::sqrt(.1) + head * std::tanh(10 * (3 * q - 2) * (radius - p1) / head)) / radius;
    const double knee = std::sqrt(std::pow(10., -3.4)), limit = std::sqrt(std::pow(10., -2.8));
    const double scale =
        radius <= knee
            ? 1.
            : (knee + (limit - knee) * std::tanh((radius - knee) / (limit - knee))) / radius;
    return {g, scale};
}

auto solve(const std::vector<std::vector<ConversionChannel>> &grids) {
    std::vector<ConversionDevice> devices;
    for (std::size_t d = 0; d < grids.size(); ++d) {
        const auto n = grids[d].size();
        auto direct = conversion_detail::zero(n);
        if (d == 1) {
            for (int bin : {1, 3}) {
                direct(index(grids[d], 1, bin), index(grids[d], 0, bin)) = -.02;
            }
        }
        FrequencyConversionModel model(1., grids[d], direct, conversion_detail::zero(n));
        devices.push_back({model,
                           std::vector<Complex>(n),
                           std::vector<Complex>(n),
                           model.zero_noise(),
                           model.zero_noise()});
    }
    const auto source = index(grids[1], 1, 1);
    devices[1].intrinsic_noise.covariance(source, source) = 1e-9;
    devices[1].intrinsic_noise.complementary(source, source) = Complex{2e-10, 1e-10};
    std::vector<Complex> offset(grids[0].size() + grids[1].size());
    offset[grids[0].size() + source] = .03;
    const CoherentHighOrderAmplifier amplifier(20., 20., 23., {0., .1});
    ConversionNonlinearDevice law{0, [=](const std::vector<Complex> &input) {
                                      return linearize_highorder_amplifier(
                                          1., grids[0], input, amplifier, {1});
                                  }};
    return solve_conversion_operating_point(
        devices, {{0, 1, 1, 0}, {0, 0, 1, 1}}, {law}, {}, {}, true, nullptr, offset);
}

void planned_feedback() {
    const CoherentHighOrderAmplifier amplifier(20., 20., 23., {0., .1});
    const auto plan = plan_conversion_frequencies(
        1.,
        {{{0, 1},
          {},
          {FrequencyHighOrderRule{
              0, 1, MemorylessPolynomial(amplifier.voltage_coefficients()), {1}}}},
         {{0, 1}, {{1, 1}}, {FrequencyCopyRule{0, 1}}}},
        {{0, 1, 1, 0}, {0, 0, 1, 1}});
    require(plan.total_channels == 8 && plan.generated_channels == 7,
            "selected feedback did not close at 8 channels");
    for (std::size_t d = 0; d < 2; ++d) {
        for (std::size_t p = 0; p < 2; ++p) {
            require(bins(plan, d, p) == std::vector<int>({1, 3}),
                    "feedback generated secondary products");
        }
    }
    const auto automatic = solve(plan.channels);
    const std::vector<std::vector<ConversionChannel>> manual{{{1, 3}, {0, 1}, {1, 1}, {0, 3}},
                                                             {{0, 3}, {1, 3}, {1, 1}, {0, 1}}};
    const auto reference = solve(manual);
    require(automatic.scaled_residual <= 1. && reference.scaled_residual <= 1.,
            "feedback did not converge");
    std::vector<std::size_t> permutation;
    std::size_t offset = 0;
    for (std::size_t d = 0; d < 2; ++d) {
        for (auto channel : plan.channels[d]) {
            permutation.push_back(offset + index(manual[d], channel.port, channel.bin));
        }
        offset += manual[d].size();
    }
    for (std::size_t i = 0; i < 8; ++i) {
        near(automatic.waves.incident[i], reference.waves.incident[permutation[i]]);
        near(automatic.waves.outgoing[i], reference.waves.outgoing[permutation[i]]);
        for (std::size_t j = 0; j < 8; ++j) {
            near(automatic.waves.outgoing_noise.covariance(i, j) / 1e-9,
                 reference.waves.outgoing_noise.covariance(permutation[i], permutation[j]) / 1e-9);
            near(automatic.waves.outgoing_noise.complementary(i, j) / 1e-9,
                 reference.waves.outgoing_noise.complementary(permutation[i], permutation[j]) /
                     1e-9);
        }
    }
    double u = .03, v = 0.;
    bool converged = false;
    for (int i = 0; i < 200; ++i) {
        const auto gain = gains(u * u + v * v);
        const double limited = gain.second * u;
        const double next_u = .03 - .02 * (gain.first * u + 7.5 * limited * limited * limited);
        const double next_v = -.02 * (gain.first * v + 2.5 * limited * limited * limited);
        converged = std::max(std::abs(next_u - u), std::abs(next_v - v)) < 1e-15;
        u = next_u;
        v = next_v;
        if (converged) {
            break;
        }
    }
    require(converged && v != 0., "independent scalar feedback reference failed");
    near(automatic.waves.incident[index(plan.channels[0], 0, 1)], u);
    near(automatic.waves.incident[index(plan.channels[0], 0, 3)], v);
    const auto gain = gains(u * u + v * v);
    // Independent time-domain cubic and DFT: only u generates; v propagates.
    for (int bin : {1, 3}) {
        Complex projection{};
        for (int k = 0; k < 128; ++k) {
            const double angle = 2 * std::acos(-1.) * k / 128;
            const double voltage = std::sqrt(100.) * gain.second * u * std::cos(angle);
            const double output =
                std::sqrt(2.) * gain.first * (u * std::cos(angle) + v * std::cos(3 * angle)) +
                .1 * voltage * voltage * voltage / std::sqrt(50.);
            projection += output * std::polar(1., -bin * angle) * std::sqrt(2.) / 128.;
        }
        near(automatic.waves.outgoing[index(plan.channels[0], 1, bin)], projection);
    }
}

void invalid_and_limits() {
    const MemorylessPolynomial quadratic({0., 0., 1.});
    for (auto selection :
         {std::vector<int>{0}, std::vector<int>{-1}, std::vector<int>{1, 1}, std::vector<int>{2}}) {
        rejects<std::invalid_argument>([&] {
            plan_conversion_frequencies(
                1., {{{0, 1}, {{0, 1}}, {FrequencyHighOrderRule{0, 1, quadratic, selection}}}}, {});
        });
    }
    for (auto polynomial : {MemorylessPolynomial({1.}), MemorylessPolynomial({0., 1.})}) {
        rejects<std::invalid_argument>([&] {
            plan_conversion_frequencies(
                1., {{{0, 1}, {{0, 1}}, {FrequencyHighOrderRule{0, 1, polynomial, {1}}}}}, {});
        });
    }
    rejects<std::invalid_argument>([&] {
        plan_conversion_frequencies(
            1., {{{0, 1}, {{0, 0}, {1, 0}}, {FrequencyHighOrderRule{0, 1, quadratic, {}}}}}, {});
    });
    ConversionFrequencyOptions options;
    options.max_bin = 1;
    rejects<std::length_error>([&] {
        plan_conversion_frequencies(
            1., {{{0, 1}, {{0, 1}}, {FrequencyHighOrderRule{0, 1, quadratic, {1}}}}}, {}, options);
    });
    options.max_bin = 2;
    options.max_transitions = 2;
    rejects<std::length_error>([&] {
        plan_conversion_frequencies(
            1., {{{0, 1}, {{0, 1}}, {FrequencyHighOrderRule{0, 1, quadratic, {1}}}}}, {}, options);
    });
    rejects<std::overflow_error>([&] {
        const int bin = std::numeric_limits<int>::max();
        plan_conversion_frequencies(
            1., {{{0, 1}, {{0, bin}}, {FrequencyHighOrderRule{0, 1, quadratic, {bin}}}}}, {});
    });
    // 128 inputs and RF outputs 1..384 fit the global 512 limit, but the
    // internal polynomial also requires DC, giving 513 channels.
    ConversionFrequencyDevice device{{0, 1}, {}, {}};
    std::vector<int> selection;
    for (int b = 1; b <= 128; ++b) {
        device.seeds.push_back({0, b});
        selection.push_back(b);
    }
    device.rules = {
        FrequencyHighOrderRule{0, 1, MemorylessPolynomial({0., 0., 1., 1.}), selection}};
    try {
        plan_conversion_frequencies(1., {device}, {});
        throw std::runtime_error("hidden DC resource limit not enforced");
    } catch (const std::length_error &error) {
        require(std::string(error.what()).find("internal polynomial") != std::string::npos,
                "resource test failed for the wrong reason");
    }
}

int main() {
    try {
        selection_and_projection();
        direct_and_eleventh_order();
        planned_feedback();
        invalid_and_limits();
        std::cout << "high-order frequency planning tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
