#include "rfmodel/linear/network.hpp"
#include "../support/test_support.hpp"
#include <random>
#include <string>

using namespace rfmodel;

// Deliberately retain the former extraction algorithm. solve() has not changed.
static SMatrix repeated_solve(const LinearNetwork &network, const std::vector<std::size_t> &ports) {
    SMatrix result{ports.size(), std::vector<Complex>(ports.size() * ports.size())};
    for (std::size_t c = 0; c < ports.size(); ++c) {
        auto copy = network;
        for (std::size_t i = 0; i < ports.size(); ++i) {
            copy.terminate(ports[i], {}, i == c ? Complex{1., 0.} : Complex{});
        }
        const auto waves = copy.solve();
        require(waves.relative_residual <= 1e-10, "legacy residual");
        for (std::size_t r = 0; r < ports.size(); ++r) {
            result(r, c) = waves.outgoing[ports[r]];
        }
    }
    return result;
}

static void compare(const LinearNetwork &network, const std::vector<std::size_t> &ports) {
    const auto expected = repeated_solve(network, ports);
    const auto actual = network.external_s(ports);
    for (std::size_t i = 0; i < actual.values.size(); ++i) {
        near(actual.values[i], expected.values[i]);
    }
}

static void failure(const LinearNetwork &network,
                    const std::vector<std::size_t> &ports,
                    const std::string &message) {
    const auto check = [&](auto action) {
        bool threw = false;
        try {
            action();
        } catch (const std::runtime_error &e) {
            threw = true;
            require(e.what() == message, "numerical failure reason changed");
        }
        require(threw, "numerical failure not rejected");
    };
    check([&] {
        network.external_s(ports);
    });
    check([&] {
        repeated_solve(network, ports);
    });
}

static void physical_reference() {
    const SMatrix first{2, {{.1, .05}, {.4, -.1}, {.7, .2}, {-.15, .03}}};
    const SMatrix second{2, {{-.2, .02}, {.3, .1}, {.6, -.15}, {.08, -.03}}};
    const std::vector<Complex> z1{{31., 12.}, {73., -21.}};
    const std::vector<Complex> z2{{19., 8.}, {110., 31.}};
    LinearNetwork network;
    network.add_power_waves(first, z1);
    network.add_power_waves(second, z2);
    network.connect(1, 2);
    compare(network, {3, 0});
    const auto actual = network.external_s({0, 3});
    // Independent physical equations: V=Z*I for each device, V1=V2,
    // I1=-I2 at the wire, V+z*I=2*sqrt(Re(z))*a at each external port.
    const auto za = s_to_z(first, z1), zb = s_to_z(second, z2);
    SMatrix equations{4, std::vector<Complex>(16)}, rhs = equations;
    for (std::size_t c = 0; c < 2; ++c) {
        equations(0, c) = za(0, c);
        equations(1, c) = za(1, c);
        equations(1, c + 2) = -zb(0, c);
        equations(3, c + 2) = zb(1, c);
    }
    equations(0, 0) += z1[0];
    equations(2, 1) = equations(2, 2) = 1.;
    equations(3, 3) += z2[1];
    rhs(0, 0) = 2. * std::sqrt(z1[0].real());
    rhs(3, 1) = 2. * std::sqrt(z2[1].real());
    const auto currents = parameter_detail::solve(equations, rhs);
    for (std::size_t c = 0; c < 2; ++c) {
        const auto v0 = za(0, 0) * currents(0, c) + za(0, 1) * currents(1, c);
        const auto v3 = zb(1, 0) * currents(2, c) + zb(1, 1) * currents(3, c);
        near(actual(0, c),
             (v0 - std::conj(z1[0]) * currents(0, c)) / (2. * std::sqrt(z1[0].real())));
        near(actual(1, c),
             (v3 - std::conj(z2[1]) * currents(3, c)) / (2. * std::sqrt(z2[1].real())));
    }
}

int main() {
    physical_reference();
    // Dense asymmetric complex feedback produces nontrivial later row pivots.
    std::mt19937 generator(41721);
    std::uniform_real_distribution<double> random(-1.5, 1.5);
    for (int trial = 0; trial < 40; ++trial) {
        constexpr std::size_t n = 12;
        SMatrix s{n, std::vector<Complex>(n * n)};
        for (auto &value : s.values) {
            value = {random(generator), random(generator)};
        }
        LinearNetwork network;
        network.add(s);
        for (std::size_t p = 0; p < n; ++p) {
            if (p != 0 && p != 3 && p != 8) {
                network.terminate(p, {.8, .2});
            }
        }
        compare(network, {0, 3, 8});
        compare(network, {8, 0, 3});
        // Both extraction methods leave the source object unassigned externally.
        rejects<std::invalid_argument>([&] {
            network.solve();
        });
        compare(network, {0, 3, 8});
    }
    LinearNetwork direct;
    const SMatrix bare{3, {{.2, .1}, .3, .4, .5, {.6, -.1}, .7, .8, .9, 1.}};
    direct.add(bare);
    const auto full = direct.external_s({2, 0, 1});
    const std::vector<std::size_t> order{2, 0, 1};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            near(full(r, c), bare(order[r], order[c]));
        }
    }
    rejects<std::invalid_argument>([&] {
        direct.external_s({});
    });
    rejects<std::invalid_argument>([&] {
        direct.external_s({0, 0});
    });
    rejects<std::invalid_argument>([&] {
        direct.external_s({0, 1});
    });
    rejects<std::out_of_range>([&] {
        direct.external_s({0, 9});
    });
    LinearNetwork empty;
    rejects<std::invalid_argument>([&] {
        empty.external_s({});
    });
    rejects<std::out_of_range>([&] {
        empty.external_s({0});
    });
    direct.terminate(2, {}, 1.);
    rejects<std::invalid_argument>([&] {
        direct.external_s({0, 1});
    });
    rejects<std::invalid_argument>([&] {
        direct.external_s({0, 1, 2});
    });
    LinearNetwork connected;
    connected.add(bare);
    connected.connect(1, 2);
    rejects<std::invalid_argument>([&] {
        connected.external_s({0, 1});
    });
    compare(connected, {0});
    for (double gap : {0., 1e-15, 1e-10}) {
        LinearNetwork near_singular;
        near_singular.add({3, {0., .2, .3, .4, 0., .5, .6, .7, 1. - gap}});
        near_singular.terminate(2, 1.);
        if (gap < 1e-12) {
            failure(near_singular, {0, 1}, "singular or ill-conditioned network");
        } else {
            compare(near_singular, {0, 1});
        }
    }
    LinearNetwork coefficient_overflow;
    coefficient_overflow.add({2, {0., 0., 0., 1e308}});
    coefficient_overflow.terminate(1, 1e308);
    failure(coefficient_overflow, {0}, "network coefficient overflow");
    // Even a non-selected outgoing port must satisfy the finite-response contract.
    LinearNetwork internal_response_overflow;
    internal_response_overflow.add({3, {0., 0., 0., 1e200, 0., 0., 1e308, 1e308, 0.}});
    internal_response_overflow.terminate(1, 1e-200);
    internal_response_overflow.terminate(2);
    failure(internal_response_overflow, {0}, "network response overflow");
    LinearNetwork second_rhs_overflow;
    second_rhs_overflow.add({3, {0., 1e308, 1e308, 0., 0., 0., 0., 1e200, 0.}});
    second_rhs_overflow.terminate(2, 1e-200);
    // First column is harmless; the second must still pass the full finite check.
    failure(second_rhs_overflow, {0, 1}, "network response overflow");
}
