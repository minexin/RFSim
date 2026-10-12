#include "rfmodel/linear_analysis.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"
#include <limits>

using namespace rfmodel;

static Complex voltage(Complex a, Complex b, Complex z) {
    return (std::conj(z) * a + z * b) / std::sqrt(z.real());
}

static Complex current(Complex a, Complex b, Complex z) {
    return (a - b) / std::sqrt(z.real());
}

static void
continuity(const NetworkWaves &w, std::size_t p, std::size_t q, Complex zp, Complex zq) {
    const auto vp = voltage(w.incident[p], w.outgoing[p], zp);
    const auto vq = voltage(w.incident[q], w.outgoing[q], zq);
    const auto ip = current(w.incident[p], w.outgoing[p], zp);
    const auto iq = current(w.incident[q], w.outgoing[q], zq);
    near(vp, vq);
    near(ip, -iq);
    near(std::norm(w.incident[p]) - std::norm(w.outgoing[p]), (vp * std::conj(ip)).real());
    near(std::norm(w.incident[p]) - std::norm(w.outgoing[p]) + std::norm(w.incident[q]) -
             std::norm(w.outgoing[q]),
         0.);
}

int main() {
    const SMatrix first{2, {{.1, .05}, {.4, -.1}, {.7, .2}, {-.15, .03}}};
    const SMatrix second{2, {{-.2, .02}, {.3, .1}, {.6, -.15}, {.08, -.03}}};
    const std::vector<Complex> z1{{31., 12.}, {73., -21.}};
    const std::vector<Complex> z2{{19., 8.}, {110., 31.}};
    const auto make = [&](const SMatrix &s1, const SMatrix &s2) {
        LinearNetwork n;
        n.add_power_waves(s1, z1);
        n.add_power_waves(s2, z2);
        n.connect(1, 2);
        return n;
    };
    auto mixed = make(first, second);
    near(mixed.port_reference_impedance_ohms(2), z2[0]);
    rejects<std::invalid_argument>([&] {
        mixed.reference_impedance_ohms();
    });
    rejects<std::out_of_range>([&] {
        mixed.port_reference_impedance_ohms(4);
    });
    // Independent coordinate-change reference: normalize both devices to 50 ohms
    // before wiring, then change only the extracted external coordinates.
    const std::vector<Complex> common{50., 50.};
    LinearNetwork normalized;
    normalized.add(renormalize_power_waves(first, z1, common).scattering);
    normalized.add(renormalize_power_waves(second, z2, common).scattering);
    normalized.connect(1, 2);
    const auto expected =
        renormalize_power_waves(normalized.external_s({0, 3}), common, {z1[0], z2[1]}).scattering;
    const auto actual = mixed.external_s({0, 3});
    for (std::size_t i = 0; i < 4; ++i) {
        near(actual.values[i], expected.values[i]);
    }
    const auto reverse = mixed.external_s({3, 0});
    near(reverse(0, 1), actual(1, 0));
    mixed.terminate(0, {.2, -.1}, {.3, .15});
    mixed.terminate(3, {-.1, .05}, {.07, -.02});
    const auto waves = mixed.solve();
    continuity(waves, 1, 2, z1[1], z2[0]);
    // Solve all four physical V/I boundary equations independently using
    // physical device Z matrices (no wave-connection coefficients).
    const auto a = s_to_z(first, z1), b = s_to_z(second, z2);
    SMatrix equations{4, std::vector<Complex>(16)}, rhs = equations;
    for (std::size_t c = 0; c < 2; ++c) {
        equations(0, c) = (1. - Complex{.2, -.1}) * a(0, c);
        equations(1, c) = a(1, c);
        equations(1, c + 2) = -b(0, c);
        equations(3, c + 2) = (1. - Complex{-.1, .05}) * b(1, c);
    }
    equations(0, 0) += z1[0] + Complex{.2, -.1} * std::conj(z1[0]);
    equations(2, 1) = equations(2, 2) = 1.;
    equations(3, 3) += z2[1] + Complex{-.1, .05} * std::conj(z2[1]);
    rhs(0, 0) = 2. * std::sqrt(z1[0].real()) * Complex{.3, .15};
    rhs(3, 0) = 2. * std::sqrt(z2[1].real()) * Complex{.07, -.02};
    const auto currents = parameter_detail::solve(equations, rhs);
    const std::vector<Complex> refs{z1[0], z1[1], z2[0], z2[1]};
    for (std::size_t p = 0; p < 4; ++p) {
        near(current(waves.incident[p], waves.outgoing[p], refs[p]), currents(p, 0));
    }
    // Ideal thru blocks have singular Z; physical wiring must still work.
    const SMatrix thru{2, {0., 1., 1., 0.}};
    auto ideal = make(renormalize_power_waves(thru, common, z1).scattering,
                      renormalize_power_waves(thru, common, z2).scattering);
    const auto ideal_expected = renormalize_power_waves(thru, common, {z1[0], z2[1]}).scattering;
    const auto ideal_actual = ideal.external_s({0, 3});
    for (std::size_t i = 0; i < 4; ++i) {
        near(ideal_actual.values[i], ideal_expected.values[i]);
    }
    // Same complex references still require a self term; conjugate references swap.
    for (auto pair : std::vector<std::vector<Complex>>{
             {{50., 20.}, {50., 20.}}, {{50., 20.}, {50., -20.}}, {25., 100.}}) {
        LinearNetwork n;
        n.add_power_waves({2, {0., .2, .3, 0.}}, {50., pair[0]});
        n.add_power_waves({2, {0., .4, .5, 0.}}, {pair[1], 50.});
        n.connect(1, 2);
        n.terminate(0, {}, .2);
        n.terminate(3, {}, Complex{.1, .1});
        continuity(n.solve(), 1, 2, pair[0], pair[1]);
    }
    // Thermal equilibrium and independent noise contributions, including
    // complex cross-port covariance from two passive physical resistors.
    IdealRLCModel r1("r1", IdealElement::Resistor, LumpedConnection::SeriesImpedance, 25.);
    IdealRLCModel r2("r2", IdealElement::Resistor, LumpedConnection::SeriesImpedance, 75.);
    const auto s1 = renormalize_power_waves(r1.s_parameters(1e9), common, z1).scattering;
    const auto s2 = renormalize_power_waves(r2.s_parameters(1e9), common, z2).scattering;
    auto thermal = make(s1, s2);
    const auto c1 = passive_thermal_noise(s1, 290.), c2 = passive_thermal_noise(s2, 290.);
    SMatrix full{4, std::vector<Complex>(16)};
    for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t c = 0; c < 2; ++c) {
            full(r, c) = c1.watts_per_hz(r, c);
            full(r + 2, c + 2) = c2.watts_per_hz(r, c);
        }
    }
    const auto noise = thermal.external_noise({0, 3}, {full}).watts_per_hz;
    const auto equilibrium = passive_thermal_noise(thermal.external_s({0, 3}), 290.).watts_per_hz;
    const auto parts = thermal.external_noise_contributions({0, 3}, {c1, c2});
    constexpr double kt = 1.380649e-23 * 290.;
    for (std::size_t i = 0; i < 4; ++i) {
        near(noise.values[i] / kt, equilibrium.values[i] / kt);
        near(noise.values[i] / kt,
             (parts[0].watts_per_hz.values[i] + parts[1].watts_per_hz.values[i]) / kt);
    }
    const auto analysis = analyze_linear(
        {{1e9, 2e9}},
        {3, 0},
        [&](double) {
            return thermal;
        },
        [&](double, const LinearNetwork &) {
            return NoiseCorrelation{full};
        });
    require(analysis.port_impedances_ohms[0] == std::vector<Complex>{z2[1], z1[0]},
            "local external references");
    const auto converted = analyze_linear(
        {{1e9}},
        {0, 3},
        [&](double) {
            return thermal;
        },
        [&](double, const LinearNetwork &) {
            return NoiseCorrelation{full};
        },
        [&](double) {
            return common;
        });
    const auto transformed =
        renormalize_noise(thermal.external_s({0, 3}), {noise}, {z1[0], z2[1]}, common);
    for (std::size_t i = 0; i < 4; ++i) {
        near(converted.noise_correlation[0].watts_per_hz.values[i] / kt,
             transformed.watts_per_hz.values[i] / kt);
    }
    // Rejected edits leave port numbering and existing boundary assignments intact.
    LinearNetwork invalid;
    for (auto refs_bad :
         std::vector<std::vector<Complex>>{{},
                                           {50.},
                                           {50., 0.},
                                           {50., {-1., 3.}},
                                           {50., {50., std::numeric_limits<double>::infinity()}}}) {
        rejects<std::invalid_argument>([&] {
            invalid.add_power_waves(thru, refs_bad);
        });
    }
    require(invalid.add(thru) == 0, "failed add changed network");
    rejects<std::invalid_argument>([&] {
        invalid.connect(0, 0);
    });
    rejects<std::out_of_range>([&] {
        invalid.connect(0, 2);
    });
    invalid.terminate(0, {}, 1.);
    invalid.terminate(1);
    near(invalid.solve().outgoing[1], 1.);
    near(invalid.reference_impedance_ohms(), 50.);
}
