#include "rfmodel/linear/frozen_small_signal.hpp"
#include "rfmodel/linear/linear_analysis.hpp"
#include <iomanip>
#include <iostream>
#include <memory>
#include <type_traits>

using namespace rfmodel;
static std::size_t checks = 0;

static void require(bool condition) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("frozen small-signal check failed: " + std::to_string(checks));
    }
}

static void near(Complex a, Complex b, double tolerance = 2e-12) {
    require(std::abs(a - b) <= tolerance * std::max({1., std::abs(a), std::abs(b)}));
}

template <class F> static void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected);
}

static FrozenSmallSignal fixture(std::size_t n, std::vector<double> g, std::vector<double> c) {
    DcDevice device;
    device.name = "analytic dense linear fixture";
    for (std::size_t i = 1; i <= n; ++i) {
        device.terminals.push_back(i);
    }
    device.evaluate = [n, g, c](const std::vector<double> &v) {
        DcDeviceEvaluation e;
        e.current_ampere.assign(n, 0.);
        e.dcurrent_dvoltage_siemens = g;
        DcChargeEvaluation q;
        q.charge_coulomb.assign(n, 0.);
        q.dcharge_dvoltage_farad = c;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                e.current_ampere[i] += g[i * n + j] * v[j];
                q.charge_coulomb[i] += c[i * n + j] * v[j];
            }
        }
        e.charge = q;
        return e;
    };
    DcNetwork network{n, {device}, std::vector<double>(n, 0.)};
    return freeze_small_signal(solve_dc(network, std::vector<double>(n, 0.)));
}

int main() {
    static_assert(!std::is_default_constructible<FrozenSmallSignal>::value,
                  "validated construction only");
    // Independent 2x2 V/I oracle: solve (I+Z Y)V=2 sqrt(Re Z) a by determinants.
    const SMatrix y{2, {{.02, .03}, {-.01, .002}, {.004, -.006}, {.03, .01}}};
    const std::vector<Complex> z{{50, 20}, {75, -15}}, incident{{.7, .2}, {-.3, .4}};
    const auto s = admittance_to_power_wave_s(y, z);
    const Complex a00 = 1. + z[0] * y(0, 0), a01 = z[0] * y(0, 1);
    const Complex a10 = z[1] * y(1, 0), a11 = 1. + z[1] * y(1, 1);
    const Complex r0 = 2. * std::sqrt(z[0].real()) * incident[0];
    const Complex r1 = 2. * std::sqrt(z[1].real()) * incident[1];
    const Complex determinant = a00 * a11 - a01 * a10;
    const std::vector<Complex> v{(r0 * a11 - a01 * r1) / determinant,
                                 (a00 * r1 - r0 * a10) / determinant};
    double wave_power = 0, vi_power = 0, vi_residual = 0;
    for (std::size_t k = 0; k < 2; ++k) {
        const auto current = y(k, 0) * v[0] + y(k, 1) * v[1];
        const auto expected = (v[k] - std::conj(z[k]) * current) / (2. * std::sqrt(z[k].real()));
        const auto outgoing = s(k, 0) * incident[0] + s(k, 1) * incident[1];
        near(outgoing, expected);
        const auto reconstructed_v =
            (std::conj(z[k]) * incident[k] + z[k] * outgoing) / std::sqrt(z[k].real());
        const auto reconstructed_i = (incident[k] - outgoing) / std::sqrt(z[k].real());
        vi_residual = std::max(vi_residual, std::abs(reconstructed_i - current));
        near(reconstructed_v, v[k]);
        wave_power += std::norm(incident[k]) - std::norm(outgoing);
        vi_power += (v[k] * std::conj(current)).real();
    }
    near(wave_power, vi_power);
    for (std::size_t n : {1u, 2u}) {
        auto open = admittance_to_power_wave_s({n, std::vector<Complex>(n * n)},
                                               std::vector<Complex>(n, {50, 20}));
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                near(open(i, j), i == j ? 1. : 0.);
            }
        }
    }
    (void)admittance_to_power_wave_s({2, {.02, -.02, -.02, .02}}, z); // singular nonzero Y
    near(admittance_to_power_wave_s({1, {1. / std::conj(z[0])}}, {z[0]})(0, 0), 0.);
    rejects([] {
        admittance_to_power_wave_s({1, {-.02}}, {{50, 0}});
    });
    rejects([&] {
        admittance_to_power_wave_s(y, {{50, 0}});
    });
    rejects([&] {
        admittance_to_power_wave_s(y, {{0, 1}, {50, 0}});
    });
    rejects([] {
        admittance_to_power_wave_s({0, {}}, {});
    });
    rejects([] {
        admittance_to_power_wave_s({2, {1.}}, {{50, 0}, {50, 0}});
    });
    rejects([] {
        admittance_to_power_wave_s({1, {{NAN, 0}}}, {{50, 0}});
    });
    // An intermediate may be outside a platform's range even when S21 is
    // representable. Correct output or explicit range rejection is acceptable;
    // silently underflowing the coupling to zero is never acceptable.
    bool extreme_checked = false;
    try {
        const auto extreme =
            admittance_to_power_wave_s({2, {0., 0., 1e-100, 0.}}, {{1e308, 0}, {1e-308, 0}});
        require(std::abs(extreme(1, 0) / Complex(-2e-100, 0) - Complex(1, 0)) < 1e-12);
        extreme_checked = true;
    } catch (const std::overflow_error &) {
        extreme_checked = true;
    }
    require(extreme_checked);
    // Non-proportional RC Schur: reducing G and C separately is incorrect.
    const auto two = fixture(2, {.003, -.002, -.002, .005}, {5e-9, -4e-9, -4e-9, 6e-9});
    const double pi = std::acos(-1.);
    const double f = 1e6;
    const Complex y1{.001, 2 * pi * f * 1e-9}, y12{.002, 2 * pi * f * 4e-9},
        y2{.003, 2 * pi * f * 2e-9};
    const auto expected = y1 + y12 * y2 / (y12 + y2);
    near(two.external_y(f, {1})(0, 0), expected);
    near(expected, {.0025494092775047916, .01470710756148325});
    near(two.external_s(f, {1}, {{50, 20}})(0, 0), {-.04278057549138256, -.7808770594538037});
    // A 1 V RMS source through 1 kohm: the .001 S source conductance is
    // already in G(1,1). Drive only its .001 A Norton injection, without
    // adding another matched termination. Reference impedances are coordinates.
    for (double frequency : {0., 1e3, 1e6}) {
        const Complex ya{.001, 2 * pi * frequency * 1e-9};
        const Complex yab{.002, 2 * pi * frequency * 4e-9};
        const Complex yb{.003, 2 * pi * frequency * 2e-9};
        const auto determinant = ya * yb + yab * (ya + yb);
        const Complex expected_v1 = .001 * (yab + yb) / determinant;
        const Complex expected_v2 = .001 * yab / determinant;
        auto driven = two.linear_network(frequency, {1, 2}, {{50, 0}, {75, 0}});
        driven.terminate(0, 1., .001 * std::sqrt(50.));
        driven.terminate(1, 1., 0.);
        const auto waves = driven.solve();
        const auto v1 = std::sqrt(50.) * (waves.incident[0] + waves.outgoing[0]);
        const auto v2 = std::sqrt(75.) * (waves.incident[1] + waves.outgoing[1]);
        const auto i1 = (waves.incident[0] - waves.outgoing[0]) / std::sqrt(50.);
        const auto i2 = (waves.incident[1] - waves.outgoing[1]) / std::sqrt(75.);
        near(v1, expected_v1);
        near(v2, expected_v2);
        near(i1, .001);
        near(i2, 0.);
        near(.001 * (1. - v1), Complex(0, 2 * pi * frequency * 1e-9) * v1 + yab * (v1 - v2));
        near(yab * (v2 - v1) + yb * v2, 0.);
        near(two.external_y(frequency, {1})(0, 0), .001 / v1);
        std::cout << std::setprecision(17) << "finite_Rs_ohm=1000 Vs_rms_volt=1 f_hz=" << frequency
                  << " V1=" << v1 << " V2=" << v2 << " source_current_ampere=" << .001 * (1. - v1)
                  << '\n';
    }
    // Three-node, two external ports plus one internal; preserve explicit order.
    const double ga = .002, gb = .003, gg = .004, gp = .001, gq = .005;
    const double ca = 2e-9, cb = 3e-9, cg = 1e-9, cp = 4e-9, cq = 5e-9;
    auto three = fixture(3,
                         {gp + ga, 0, -ga, 0, gq + gb, -gb, -ga, -gb, ga + gb + gg},
                         {cp + ca, 0, -ca, 0, cq + cb, -cb, -ca, -cb, ca + cb + cg});
    Complex ya{ga, 2 * pi * f * ca}, yb{gb, 2 * pi * f * cb}, yg{gg, 2 * pi * f * cg};
    Complex yp{gp, 2 * pi * f * cp}, yq{gq, 2 * pi * f * cq}, d = ya + yb + yg;
    auto reduced = three.external_y(f, {1, 2});
    near(reduced(0, 0), yp + ya - ya * ya / d);
    near(reduced(1, 1), yq + yb - yb * yb / d);
    near(reduced(0, 1), -ya * yb / d);
    near(reduced(1, 0), -ya * yb / d);
    auto normal = three.external_s(f, {1, 2}, z),
         reversed = three.external_s(f, {2, 1}, {z[1], z[0]});
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            near(normal(i, j), reversed(1 - i, 1 - j));
        }
    }
    auto full = three.nodal_y(f), reordered = three.external_y(f, {3, 1, 2});
    const std::size_t order[] = {2, 0, 1};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            near(reordered(i, j), full(order[i], order[j]));
        }
    }
    for (const auto &nodes : std::vector<std::vector<DcNode>>{{}, {0}, {4}, {1, 1}}) {
        rejects([&] {
            three.external_y(f, nodes);
        });
    }
    for (double bad : {-1., double(INFINITY), double(NAN), std::numeric_limits<double>::max()}) {
        rejects([&] {
            three.nodal_y(bad);
        });
    }
    auto singular_internal = fixture(2, {0, 1, 1, 0}, {0, 0, 0, 0});
    (void)singular_internal.nodal_y(0);
    rejects([&] {
        singular_internal.external_y(0, {1});
    });
    // Two internal active nodes cancel only at omega*c=1; full DC G invertible.
    auto frequency_singular = fixture(3, {2, 1, 0, 1, 1, 0, 0, 0, -1}, {0, 0, 0, 0, 0, 1, 0, 1, 0});
    (void)frequency_singular.external_y(0, {1});
    (void)frequency_singular.external_y(.5 / pi, {1, 2, 3});
    rejects([&] {
        frequency_singular.external_y(.5 / pi, {1});
    });
    auto analysis = analyze_linear({{0, 1e3, 1e6}}, {0, 1}, [three, z](double frequency) {
        return three.linear_network(frequency, {2, 1}, z);
    });
    require(analysis.noise_correlation.empty());
    require(analysis.scattering.size() == 3);
    require(analysis.external_ports == std::vector<std::size_t>({0, 1}));
    require(analysis.port_impedances_ohms[2] == z);
    auto expected_s = three.external_s(1e6, {2, 1}, z);
    for (std::size_t i = 0; i < 4; ++i) {
        near(analysis.scattering[2].values[i], expected_s.values[i]);
    }
    bool frequency_context = false;
    try {
        (void)analyze_linear({{0, .5 / pi}}, {0}, [frequency_singular](double frequency) {
            return frequency_singular.linear_network(frequency, {1}, {{50, 0}});
        });
    } catch (const std::exception &error) {
        frequency_context = std::string(error.what()).find("Hz") != std::string::npos;
    }
    require(frequency_context);
    // Numeric snapshot survives callback dependencies and is never reevaluated.
    int calls = 0;
    double coefficient = .002;
    auto owned = [&] {
        DcDevice device{"mutable coefficient", {1}, [&](const std::vector<double> &voltage) {
                            ++calls;
                            return DcDeviceEvaluation{
                                {coefficient * voltage[0]}, {coefficient}, std::nullopt};
                        }};
        DcNetwork network{1, {device}, {.004}};
        auto point = solve_dc(network, {0});
        near(point.voltage_volt()[0], 2.);
        return freeze_small_signal(point);
    }();
    int accepted_calls = calls;
    coefficient = 100.;
    near(owned.nodal_y(0)(0, 0), .002);
    (void)analyze_linear({{0, 1e3}}, {0}, [owned](double frequency) {
        return owned.linear_network(frequency, {1}, {{50, 0}});
    });
    require(calls == accepted_calls);
    // Bias-dependent charge uses dQ/dV at the accepted bias, never Q/V.
    DcDevice nonlinear{
        "cubic current and charge", {1}, [](const std::vector<double> &volts) {
            double v = volts[0];
            return DcDeviceEvaluation{
                {.002 * v + .002 * v * v * v},
                {.002 + .006 * v * v},
                DcChargeEvaluation{{1e-9 * v + 3e-9 * v * v * v}, {1e-9 + 9e-9 * v * v}}};
        }};
    auto nonlinear_point = solve_dc(DcNetwork{1, {nonlinear}, {.002}}, {0});
    auto biased = freeze_small_signal(nonlinear_point);
    // Independent monotone bisection for v^3+v-1=0.
    double lo = 0, hi = 2;
    for (int k = 0; k < 100; ++k) {
        double mid = (lo + hi) / 2;
        if (mid * mid * mid + mid > 1) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    double root = (lo + hi) / 2, gbias = .002 + .006 * root * root,
           cbias = 1e-9 + 9e-9 * root * root;
    near(nonlinear_point.voltage_volt()[0], root);
    for (double frequency : {0., 1e3, 1e6}) {
        near(biased.nodal_y(frequency)(0, 0), {gbias, 2 * pi * frequency * cbias});
    }
    require(std::abs(cbias - (1e-9 + 3e-9 * root * root)) > 1e-9);
    // Small physical conductances do not inherit a universal 1 S pivot floor.
    auto tiny = fixture(2, {2e-15, -1e-15, -1e-15, 2e-15}, {0, 0, 0, 0});
    require(std::abs(tiny.external_y(0, {1})(0, 0).real() - 1.5e-15) < 1e-29);
    // Do not silently quantize a subnormal public off-diagonal S component.
    rejects([] {
        admittance_to_power_wave_s({2, {{0, 0}, {1e-312, 0}, {0, 0}, {0, 0}}}, {{50, 0}, {50, 0}});
    });
    // Frequency results overflow explicitly rather than returning infinities.
    auto huge_c = fixture(1, {1}, {std::numeric_limits<double>::max()});
    rejects([&] {
        huge_c.nodal_y(1);
    });
    near(owned.nodal_y(0)(0, 0), .002); // prior snapshot remains intact after failures
    std::cout << std::setprecision(17) << "frozen small-signal focused tests passed: " << checks
              << " checks; VI residual=" << vi_residual
              << " A; power residual=" << std::abs(wave_power - vi_power)
              << " W; wave power=" << wave_power << " W\n";
}
