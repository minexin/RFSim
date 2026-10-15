#include "rfmodel/linear/frozen_small_signal.hpp"
#include "../support/test_support.hpp"
#include <iostream>
#include <memory>

using namespace rfmodel;
constexpr double kt = 1.380649e-23 * 290.;

FrozenSmallSignal fixture(const std::vector<double> &g,
                          const std::vector<double> &c,
                          std::shared_ptr<int> calls = std::make_shared<int>(0)) {
    const auto n = static_cast<std::size_t>(std::sqrt(g.size()));
    DcDevice device;
    device.name = "linear RC";
    for (std::size_t i = 1; i <= n; ++i) {
        device.terminals.push_back(i);
    }
    device.evaluate = [g, c, calls, n](const std::vector<double> &v) {
        ++*calls;
        DcDeviceEvaluation result{std::vector<double>(n), g, std::nullopt};
        DcChargeEvaluation charge{std::vector<double>(n), c};
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                result.current_ampere[i] += g[i * n + j] * v[j];
                charge.charge_coulomb[i] += c[i * n + j] * v[j];
            }
        }
        result.charge = charge;
        return result;
    };
    return freeze_small_signal(
        solve_dc({n, {device}, std::vector<double>(n)}, std::vector<double>(n)));
}

void thermal_and_internal() {
    // A series resistor and grounded shunt resistor, both at T, with capacitors.
    // Branch current noise has opposite signs at the two nodes.
    const auto model = fixture({.02, -.02, -.02, .03}, {1e-9, -1e-9, -1e-9, 3e-9});
    NodalCurrentNoise noise{{2, {4 * kt * .02, -4 * kt * .02, -4 * kt * .02, 4 * kt * .03}}};
    for (double f : {0., 1e5, 2e6}) {
        for (Complex z : {Complex{50., 0.}, Complex{75., 15.}}) {
            if (f == 0. && z.imag() != 0.) {
                continue;
            }
            const auto actual = model.external_noise(f, {1}, {z}, noise);
            const Complex ys{.02, 2 * std::acos(-1.) * f * 1e-9};
            const Complex yg{.01, 2 * std::acos(-1.) * f * 2e-9};
            const Complex load = 1. / z;
            const Complex det = (ys + load) * (ys + yg) - ys * ys;
            const Complex h0 = std::sqrt(z.real()) / z * (ys + yg) / det;
            const Complex h1 = std::sqrt(z.real()) / z * ys / det;
            const double expected = 4 * kt * (.02 * std::norm(h0 - h1) + .01 * std::norm(h1));
            near(actual.watts_per_hz(0, 0) / kt, expected / kt);
            const auto s = model.external_s(f, {1}, {z});
            near(actual.watts_per_hz(0, 0) / kt, 1. - std::norm(s(0, 0)));
        }
    }
    // The full multiport covariance must agree with the passive equilibrium law.
    const std::vector<Complex> refs{{75., 15.}, {40., -7.}};
    const auto actual = model.external_noise(2e5, {2, 1}, refs, noise);
    const auto expected = passive_thermal_noise(model.external_s(2e5, {2, 1}, refs), 290.);
    for (std::size_t i = 0; i < 4; ++i) {
        near(actual.watts_per_hz.values[i] / kt, expected.watts_per_hz.values[i] / kt);
    }
}

void correlated_active_and_ownership() {
    const auto calls = std::make_shared<int>(0);
    const auto model = fixture({.02, -.005, .007, .03}, {1e-9, .2e-9, -.1e-9, 2e-9}, calls);
    const int accepted_calls = *calls;
    const Complex u0{1., .3}, u1{-.7, .2};
    NodalCurrentNoise q{{2,
                         {kt * std::norm(u0),
                          kt * u0 * std::conj(u1),
                          kt * u1 * std::conj(u0),
                          kt * std::norm(u1)}}};
    const double f = 1e6, w = 2 * std::acos(-1.) * f;
    const Complex z0{40., 10.}, z1{90., -30.};
    const Complex a{.02, w * 1e-9}, b{-.005, w * .2e-9};
    const Complex c{.007, -w * .1e-9}, d{.03, w * 2e-9};
    const Complex aa = a + 1. / z0, dd = d + 1. / z1, det = aa * dd - b * c;
    const Complex v0 = (dd * u0 - b * u1) / det, v1 = (aa * u1 - c * u0) / det;
    const Complex out0 = std::sqrt(z0.real()) / z0 * v0;
    const Complex out1 = std::sqrt(z1.real()) / z1 * v1;
    const auto actual = model.external_noise(f, {2, 1}, {z1, z0}, q);
    near(actual.watts_per_hz(0, 0) / kt, std::norm(out1));
    near(actual.watts_per_hz(1, 1) / kt, std::norm(out0));
    near(actual.watts_per_hz(0, 1) / kt, out1 * std::conj(out0));
    require(*calls == accepted_calls, "frozen noise reevaluated device callback");
    // Exact low-rank cancellation through an internal node remains zero.
    const auto cancel = fixture({2., -1., -1., 2.}, {0., 0., 0., 0.});
    const auto zero = cancel.external_noise(1., {1}, {1.}, {{2, {kt, -2 * kt, -2 * kt, 4 * kt}}});
    require(zero.watts_per_hz(0, 0).real() < kt * 1e-28, "correlation cancellation lost");
}

void failure_contracts() {
    const auto model = fixture({.02, 0., 0., .03}, {0., 0., 0., 0.});
    const NodalCurrentNoise q{{2, {kt, 0., 0., kt}}};
    for (auto ports : std::vector<std::vector<DcNode>>{{}, {0}, {3}, {1, 1}}) {
        rejects<std::invalid_argument>([&] {
            model.external_noise(1., ports, {50.}, q);
        });
    }
    rejects<std::invalid_argument>([&] {
        model.external_noise(-1., {1}, {50.}, q);
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(1., {1}, {-50.}, q);
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(1., {1}, {}, q);
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(1., {1}, {50.}, {{1, {kt}}});
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(1., {1}, {50.}, {{2, {-kt, 0., 0., kt}}});
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(1., {1}, {50.}, {{2, {kt, kt, 0., kt}}});
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(0., {1}, {{50., 1.}}, q);
    });
    rejects<std::invalid_argument>([&] {
        model.external_noise(
            0., {1}, {50.}, {{2, {kt, Complex{0., .5 * kt}, Complex{0., -.5 * kt}, kt}}});
    });
    // Zero noise does not hide a singular reference-loaded wave boundary.
    const auto active = fixture({-.02}, {0.});
    rejects<std::domain_error>([&] {
        active.external_noise(1., {1}, {50.}, {{1, {0.}}});
    });
    // The loaded physical circuit can be solvable when an open-circuit internal
    // Schur block is singular. Noise uses the full nodal boundary, not Yii^-1.
    const auto internal_singular = fixture({2., -1., -1., 0.}, {0., 0., 0., 0.});
    rejects<std::domain_error>([&] {
        internal_singular.external_y(1., {1});
    });
    near(internal_singular.external_noise(1., {1}, {1.}, q).watts_per_hz(0, 0) / kt, 1.);
}

int main() {
    try {
        thermal_and_internal();
        correlated_active_and_ownership();
        failure_contracts();
        std::cout << "frozen nodal noise tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
