#include "rfmodel/mna/power_wave_s.hpp"

#include "rfmodel/core/power_wave_reference.hpp"
#include "rfmodel/linear/frozen_small_signal.hpp"
#include "rfmodel/linear/network.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace rfmodel::mna;
using rfmodel::SMatrix;

std::size_t checks = 0;
constexpr double pi = 3.1415926535897932384626433832795;
const double omega_one_hz = 1.0 / (2.0 * pi);
const Complex z1{4, 3}, z2{9, -2};
const std::vector<Complex> complex_references{z1, z2};

// These rational constants come from the independent, hand-written exact
// matrices in mna-port-design/oracle.py and oracle-results.json, not from an
// implementation stamper, numerical solver, or a Y-to-S conversion under test.
const SMatrix expected_through{
    2, {Complex{33, 4} / 85., Complex{78, -6} / 85., Complex{78, -6} / 85., Complex{-32, 9} / 85.}};
const SMatrix expected_rlc{2,
                           {Complex{169, 808} / 1405.,
                            Complex{-72, -444} / 1405.,
                            Complex{-72, -444} / 1405.,
                            Complex{-809, -1008} / 1405.}};
const Complex expected_short = Complex{-7, 24} / 25.;
const Complex expected_two_ohms = Complex{-1, 8} / 15.;

void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(Complex actual,
          Complex expected,
          const char *message,
          double relative = 3e-11,
          double absolute = 1e-13) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()) &&
                std::abs(actual - expected) <= absolute + relative * std::abs(expected),
            message);
}

void matrix_near(const SMatrix &actual, const SMatrix &expected, const char *message) {
    require(actual.ports == expected.ports && actual.values.size() == expected.values.size(),
            "scattering matrix has the requested owning row-major shape");
    for (std::size_t row = 0; row < expected.ports; ++row) {
        for (std::size_t column = 0; column < expected.ports; ++column) {
            near(actual(row, column), expected(row, column), message);
        }
    }
}

template <class F> void fails(F action, Failure expected, const char *message) {
    ++checks;
    try {
        action();
    } catch (const SolveError &error) {
        ++checks;
        if (error.failure() != expected) {
            throw std::runtime_error(std::string(message) +
                                     ": unexpected failure category: " + error.what());
        }
        require(std::string(error.what()).size() > 0, "structured failure explains the rejection");
        return;
    }
    throw std::runtime_error(message);
}

void diagnostics(const PowerWaveSResult &result,
                 const std::vector<Complex> &references,
                 std::size_t augmented_unknowns,
                 const SolveOptions &options = {}) {
    require(result.scattering.ports == references.size(), "one S port per requested boundary");
    require(result.scattering.values.size() == references.size() * references.size(),
            "S owns all matrix entries");
    require(result.references_ohm == references, "references preserve exact caller ordering");
    require(result.column_diagnostics.size() == references.size(),
            "each incident column has its own diagnostics");
    const double floor = std::max(options.pivot_relative_tolerance,
                                  64 * std::numeric_limits<double>::epsilon() *
                                      static_cast<double>(augmented_unknowns));
    for (const auto &d : result.column_diagnostics) {
        require(std::isfinite(d.max_scaled_residual) && d.max_scaled_residual >= 0 &&
                    d.max_scaled_residual <= options.backward_relative_tolerance,
                "each column passed the normalized backward residual gate");
        require(std::isfinite(d.max_kcl_residual_a) && d.max_kcl_residual_a >= 0,
                "each column reports finite original-unit KCL residual");
        require(std::isfinite(d.max_constraint_residual_v) && d.max_constraint_residual_v >= 0,
                "each column reports finite original-unit voltage-boundary residual");
        require(d.effective_pivot_tolerance == floor,
                "pivot roundoff floor uses augmented rather than bare dimension");
        require(std::isfinite(d.min_scaled_pivot) && d.min_scaled_pivot > floor,
                "every admitted pivot exceeds the effective gate");
        require(d.refinement_steps <= 3, "each RHS obeys the fixed refinement budget");
        require(d.min_equilibration_exponent <= d.max_equilibration_exponent,
                "equilibration exponent diagnostics are ordered");
        const auto &first = result.column_diagnostics.front();
        require(d.min_scaled_pivot == first.min_scaled_pivot &&
                    d.effective_pivot_tolerance == first.effective_pivot_tolerance &&
                    d.min_equilibration_exponent == first.min_equilibration_exponent &&
                    d.max_equilibration_exponent == first.max_equilibration_exponent,
                "columns report the same shared factorization diagnostics");
    }
}

void unitary(const SMatrix &scattering) {
    for (std::size_t row = 0; row < scattering.ports; ++row) {
        for (std::size_t column = 0; column < scattering.ports; ++column) {
            Complex inner{};
            for (std::size_t k = 0; k < scattering.ports; ++k) {
                inner += std::conj(scattering(k, row)) * scattering(k, column);
            }
            near(inner, row == column ? 1. : 0., "lossless fixture is power-wave unitary");
        }
    }
}

double absorbed_wave_power(const SMatrix &scattering) {
    // The exact oracle uses this simultaneous complex excitation, not merely
    // basis columns: a[k] = k+1 + j(2-k).
    double power = 0;
    for (std::size_t row = 0; row < scattering.ports; ++row) {
        const Complex incident{static_cast<double>(row + 1), 2. - static_cast<double>(row)};
        Complex outgoing{};
        for (std::size_t column = 0; column < scattering.ports; ++column) {
            outgoing += scattering(row, column) *
                        Complex{static_cast<double>(column + 1), 2. - static_cast<double>(column)};
        }
        power += std::norm(incident) - std::norm(outgoing);
    }
    return power;
}

void open_short_and_one_port_oracles() {
    NetworkBuilder open;
    const auto terminal = open.add_node();
    const auto empty = open.finalize();
    fails(
        [&] {
            (void)solve_ac(empty, omega_one_hz);
        },
        Failure::singular_or_ill_conditioned,
        "bare open is singular before a reference boundary is added");
    const auto result = extract_power_wave_s(empty, omega_one_hz, {{terminal, ground}}, {z1});
    matrix_near(result.scattering, SMatrix{1, {1.}}, "loaded open has S=1");
    diagnostics(result, {z1}, 2);
    near(absorbed_wave_power(result.scattering), 0., "open absorbs no real power");

    for (const auto reference : {Complex{4, 0}, z1}) {
        const Complex expected = reference == z1 ? expected_short : Complex{-1, 0};
        NetworkBuilder shorted;
        const auto n = shorted.add_node();
        shorted.add_voltage_source(n, ground, {});
        const auto ideal =
            extract_power_wave_s(shorted.finalize(), omega_one_hz, {{n, ground}}, {reference});
        matrix_near(ideal.scattering, SMatrix{1, {expected}}, "retained ideal-source short");
        diagnostics(ideal, {reference}, 3);
        unitary(ideal.scattering);

        NetworkBuilder ground_only;
        const auto same = extract_power_wave_s(
            ground_only.finalize(), omega_one_hz, {{ground, ground}}, {reference});
        matrix_near(same.scattering, SMatrix{1, {expected}}, "ground-ground port is a short");
        diagnostics(same, {reference}, 1);

        // A same-node non-ground port is valid when the otherwise invisible
        // node has a physical reference. Additive incidence must cancel.
        NetworkBuilder referenced;
        const auto r = referenced.add_node();
        referenced.add_resistor(r, ground, 7);
        const auto probe =
            extract_power_wave_s(referenced.finalize(), omega_one_hz, {{r, r}}, {reference});
        matrix_near(probe.scattering, SMatrix{1, {expected}}, "same-node incidence cancels");
        diagnostics(probe, {reference}, 2);
    }

    NetworkBuilder match;
    const auto input = match.add_node(), capacitor = match.add_node();
    match.add_resistor(input, capacitor, 4);
    match.add_capacitor(capacitor, ground, 1. / 3.);
    const auto matched =
        extract_power_wave_s(match.finalize(), omega_one_hz, {{input, ground}}, {z1});
    matrix_near(matched.scattering, SMatrix{1, {0.}}, "conjugate-matched RC has S=0");
    near(absorbed_wave_power(matched.scattering), 5., "matched RC absorbs all incident power");

    NetworkBuilder differential;
    const auto positive = differential.add_node(), negative = differential.add_node();
    differential.add_resistor(positive, ground, 2);
    differential.add_resistor(negative, ground, 3);
    const auto pair =
        extract_power_wave_s(differential.finalize(), omega_one_hz, {{positive, negative}}, {z1});
    matrix_near(pair.scattering,
                SMatrix{1, {Complex{1. / 5., 4. / 15.}}},
                "referenced differential port sees exact five-ohm impedance");
    diagnostics(pair, {z1}, 3);
    near(absorbed_wave_power(pair.scattering), 40. / 9., "differential fixture power oracle");

    NetworkBuilder lc;
    const auto resonant = lc.add_node();
    lc.add_inductor(resonant, ground, 1);
    lc.add_capacitor(resonant, ground, 1);
    const auto resonance = lc.finalize();
    fails(
        [&] {
            (void)solve_ac(resonance, omega_one_hz);
        },
        Failure::singular_or_ill_conditioned,
        "bare parallel LC has a singular resonant state");
    const auto loaded = extract_power_wave_s(resonance, omega_one_hz, {{resonant, ground}}, {z1});
    matrix_near(loaded.scattering, SMatrix{1, {1.}}, "port loading resolves LC resonance");
    diagnostics(loaded, {z1}, 3);
}

void duplicate_reversed_dependent_and_ordered_ports() {
    NetworkBuilder builder;
    const auto n = builder.add_node();
    const auto network = builder.finalize();
    const auto through =
        extract_power_wave_s(network, omega_one_hz, {{n, ground}, {n, ground}}, complex_references);
    matrix_near(through.scattering, expected_through, "duplicate terminal pairs form through");
    diagnostics(through, complex_references, 3);
    unitary(through.scattering);

    NetworkBuilder distinct;
    const auto a = distinct.add_node(), b = distinct.add_node();
    distinct.add_voltage_source(a, b, {});
    const auto ideal = extract_power_wave_s(
        distinct.finalize(), omega_one_hz, {{a, ground}, {b, ground}}, complex_references);
    matrix_near(ideal.scattering, expected_through, "distinct terminals with ideal short through");
    diagnostics(ideal, complex_references, 5);

    const auto reversed =
        extract_power_wave_s(network, omega_one_hz, {{n, ground}, {ground, n}}, complex_references);
    for (std::size_t row = 0; row < 2; ++row) {
        for (std::size_t column = 0; column < 2; ++column) {
            near(reversed.scattering(row, column),
                 (row == column ? 1. : -1.) * expected_through(row, column),
                 "reversing one port flips its row and column signs");
        }
    }
    unitary(reversed.scattering);

    NetworkBuilder delta;
    const auto p = delta.add_node(), q = delta.add_node();
    const auto delta_network = delta.finalize();
    const std::vector<Port> ports{{p, ground}, {q, ground}, {p, q}};
    const SMatrix expected_delta{
        3, {1. / 3., 2. / 3., 2. / 3., 2. / 3., 1. / 3., -2. / 3., 2. / 3., -2. / 3., 1. / 3.}};
    const auto dependent = extract_power_wave_s(delta_network, omega_one_hz, ports, {4., 4., 4.});
    matrix_near(dependent.scattering, expected_delta, "rank-deficient port incidence is valid");
    diagnostics(dependent, {4., 4., 4.}, 5);
    unitary(dependent.scattering);

    const std::array<std::size_t, 3> permutation{{2, 0, 1}};
    const auto ordered = extract_power_wave_s(
        delta_network, omega_one_hz, {ports[2], ports[0], ports[1]}, {4., 4., 4.});
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            near(ordered.scattering(row, column),
                 expected_delta(permutation[row], permutation[column]),
                 "caller port ordering permutes both S dimensions without sorting");
        }
    }

    // Swap genuinely distinct terminals together with unequal complex refs.
    const auto swapped = extract_power_wave_s(
        distinct.finalize(), omega_one_hz, {{b, ground}, {a, ground}}, {z2, z1});
    diagnostics(swapped, {z2, z1}, 5);
    for (std::size_t row = 0; row < 2; ++row) {
        for (std::size_t column = 0; column < 2; ++column) {
            near(swapped.scattering(row, column),
                 expected_through(1 - row, 1 - column),
                 "unequal complex references follow reordered ports");
        }
    }
}

Network make_rlc(double impedance_scale = 1.) {
    NetworkBuilder builder;
    const auto input = builder.add_node(), middle = builder.add_node(), output = builder.add_node();
    builder.add_resistor(input, middle, 2 * impedance_scale);
    builder.add_inductor(middle, output, 3 * impedance_scale);
    builder.add_capacitor(output, ground, .5 / impedance_scale);
    return builder.finalize();
}

void resistor_rlc_and_physical_scaling() {
    NetworkBuilder resistor;
    const auto a = resistor.add_node(), b = resistor.add_node();
    resistor.add_resistor(a, b, 6);
    const auto network = resistor.finalize();
    fails(
        [&] {
            (void)solve_ac(network, omega_one_hz);
        },
        Failure::singular_or_ill_conditioned,
        "bare series resistor has a gauge but its loaded S is valid");
    const SMatrix expected_resistor{2, {11. / 19., 12. / 19., 12. / 19., 1. / 19.}};
    const auto series =
        extract_power_wave_s(network, omega_one_hz, {{a, ground}, {b, ground}}, {4., 9.});
    matrix_near(series.scattering, expected_resistor, "singular finite bare Y series resistor");
    diagnostics(series, {4., 9.}, 4);
    near(absorbed_wave_power(series.scattering), 408. / 361., "series resistor power oracle");

    const std::vector<Port> ports{{NodeId{1}, ground}, {NodeId{3}, ground}};
    const auto rlc = make_rlc();
    const auto extracted = extract_power_wave_s(rlc, omega_one_hz, ports, complex_references);
    matrix_near(extracted.scattering, expected_rlc, "RLC exact independent two-port oracle");
    diagnostics(extracted, complex_references, 6);
    near(extracted.scattering(0, 1), extracted.scattering(1, 0), "RLC reciprocity");
    near(absorbed_wave_power(extracted.scattering), 4768. / 1405., "RLC exact loss oracle");
    // For a reciprocal passive 2-port, the Hermitian loss matrix is PSD.
    const auto &s = extracted.scattering;
    const double h00 = 1 - std::norm(s(0, 0)) - std::norm(s(1, 0));
    const double h11 = 1 - std::norm(s(0, 1)) - std::norm(s(1, 1));
    const Complex h01 = -std::conj(s(0, 0)) * s(0, 1) - std::conj(s(1, 0)) * s(1, 1);
    require(h00 >= -1e-12 && h11 >= -1e-12 && h00 * h11 - std::norm(h01) >= -1e-12,
            "RLC absorption matrix is positive semidefinite");
    near(h00 * h11 - std::norm(h01), 0., "single resistor gives rank-one dissipative loss");

    for (double scale : {1e-9, 1e-3, 1., 1e3, 1e9}) {
        const std::vector<Complex> refs{z1 * scale, z2 * scale};
        const auto scaled = extract_power_wave_s(make_rlc(scale), omega_one_hz, ports, refs);
        matrix_near(scaled.scattering, expected_rlc, "uniform physical impedance scaling");
        diagnostics(scaled, refs, 6);
    }
    for (double voltage_scale : {1e-6, 1., 1e6}) {
        for (double current_scale : {1e-9, 1e-3, 1e3}) {
            SolveOptions options;
            options.voltage_scale_v = voltage_scale;
            options.current_scale_a = current_scale;
            const auto scaled =
                extract_power_wave_s(rlc, omega_one_hz, ports, complex_references, options);
            matrix_near(scaled.scattering, expected_rlc, "physical unit scales preserve S");
            diagnostics(scaled, complex_references, 6, options);
        }
    }
}

Complex physical_affine_output(bool voltage_source, Complex incident) {
    // Independent physical experiment: realize reference 4+j3 as series R=4,
    // L=3 driven by a 4*a volt Thevenin source. No augmented-wave implementation
    // is reproduced here. Existing solve_ac returns the actual affine response.
    NetworkBuilder builder;
    const auto output = builder.add_node();
    if (voltage_source) {
        const auto supply = builder.add_node();
        builder.add_voltage_source(supply, ground, {123, {7, 5}});
        builder.add_resistor(supply, output, 2);
    } else {
        builder.add_resistor(output, ground, 2);
        builder.add_current_source(output, ground, {123, {3, 2}});
    }
    const auto generator = builder.add_node(), middle = builder.add_node();
    builder.add_voltage_source(generator, ground, {0, 4. * incident});
    builder.add_resistor(generator, middle, 4);
    const auto feed = builder.add_inductor(middle, output, 3);
    const auto response = solve_ac(builder.finalize(), omega_one_hz);
    return (response.node_voltage[output.value] -
            std::conj(z1) * response.elements[feed.value].current) /
           4.;
}

void sources_are_incremental_and_unchanged() {
    for (bool voltage : {false, true}) {
        for (const auto drive : {SourceValue{},
                                 SourceValue{123, {7, 5}},
                                 SourceValue{-789, {-2, 11}},
                                 SourceValue{1e160, {1e160, -1e160}}}) {
            NetworkBuilder builder;
            const auto output = builder.add_node();
            ElementId source{0};
            if (voltage) {
                const auto supply = builder.add_node();
                source = builder.add_voltage_source(supply, ground, drive);
                builder.add_resistor(supply, output, 2);
            } else {
                builder.add_resistor(output, ground, 2);
                source = builder.add_current_source(output, ground, drive);
            }
            const auto network = builder.finalize();
            const auto result =
                extract_power_wave_s(network, omega_one_hz, {{output, ground}}, {z1});
            matrix_near(result.scattering,
                        SMatrix{1, {expected_two_ohms}},
                        "independent source values do not enter incremental S");
            diagnostics(result, {z1}, voltage ? 4 : 2);
            require(network.elements()[source.value].source.dc == drive.dc &&
                        network.elements()[source.value].source.ac_rms == drive.ac_rms,
                    "extraction never edits the owning source snapshot");
        }
        const Complex b0 = physical_affine_output(voltage, 0.);
        const Complex b1 = physical_affine_output(voltage, 1.);
        near(b0,
             voltage ? Complex{38. / 15., 2. / 5.} : Complex{-32. / 15., -4. / 15.},
             "physical source response has the independently known nonzero affine offset");
        near(b1 - b0, expected_two_ohms, "difference of physical responses equals incremental S");
        require(std::abs(b1 - expected_two_ohms) > .1,
                "raw driven b/a is deliberately distinguished from incremental S");
        const Complex arbitrary{2, -3};
        near(physical_affine_output(voltage, arbitrary) - b0,
             arbitrary * expected_two_ohms,
             "incremental coefficient applies to arbitrary complex incident amplitude");
    }

    // Neither an overflowing physical RHS nor unrelated element-power overflow
    // is part of incremental S extraction. Source topology must still remain.
    NetworkBuilder huge;
    const auto supply = huge.add_node(), output = huge.add_node();
    const double maximum = std::numeric_limits<double>::max();
    huge.add_voltage_source(supply, ground, {maximum, {maximum, maximum}});
    huge.add_resistor(supply, output, 2);
    huge.add_current_source(output, ground, {maximum, {maximum, maximum}});
    huge.add_current_source(output, ground, {maximum, {maximum, maximum}});
    const auto huge_network = huge.finalize();
    fails(
        [&] {
            (void)solve_ac(huge_network, omega_one_hz);
        },
        Failure::range_error,
        "the physical source response cannot be evaluated in the supported range");
    matrix_near(
        extract_power_wave_s(huge_network, omega_one_hz, {{output, ground}}, {z1}).scattering,
        SMatrix{1, {expected_two_ohms}},
        "incremental assembly zeros sources before arithmetic");

    NetworkBuilder current_only;
    const auto current_node = current_only.add_node();
    current_only.add_current_source(current_node, ground, {5, {3, 2}});
    matrix_near(
        extract_power_wave_s(current_only.finalize(), omega_one_hz, {{current_node, ground}}, {z1})
            .scattering,
        SMatrix{1, {1.}},
        "zeroed current source leaves a loaded open");
}

void nonunique_loaded_states_and_legal_inductor_cycles() {
    const auto reject = [](const Network &network, const std::vector<Port> &ports) {
        fails(
            [&] {
                (void)extract_power_wave_s(
                    network, omega_one_hz, ports, std::vector<Complex>(ports.size(), z1));
            },
            Failure::singular_or_ill_conditioned,
            "nonunique loaded full state must fail even when port S could be unique");
    };
    {
        NetworkBuilder floating;
        const auto a = floating.add_node(), b = floating.add_node();
        floating.add_resistor(a, b, 5);
        // A grounded independent I source does not anchor voltage after zeroing.
        floating.add_current_source(ground, a, {1, {1, 0}});
        reject(floating.finalize(), {{a, b}});
        reject(floating.finalize(), {{a, b}, {b, a}, {a, a}});
        const auto anchored = extract_power_wave_s(
            floating.finalize(), omega_one_hz, {{a, b}, {a, ground}}, {z1, z2});
        diagnostics(anchored, complex_references, 4);
    }
    for (bool reverse : {false, true}) {
        NetworkBuilder parallel;
        const auto n = parallel.add_node();
        parallel.add_voltage_source(n, ground, {7, {7, 5}});
        parallel.add_voltage_source(reverse ? ground : n, reverse ? n : ground, {-3, {2, -1}});
        reject(parallel.finalize(), {{n, ground}});
    }
    {
        NetworkBuilder cycle;
        const auto a = cycle.add_node(), b = cycle.add_node();
        cycle.add_voltage_source(a, ground, {1, {1, 0}});
        cycle.add_voltage_source(b, a, {2, {2, 0}});
        cycle.add_voltage_source(b, ground, {3, {3, 0}});
        reject(cycle.finalize(), {{a, ground}, {b, ground}});
    }
    {
        NetworkBuilder hidden;
        const auto visible = hidden.add_node(), resonant = hidden.add_node();
        hidden.add_resistor(visible, ground, 2);
        hidden.add_inductor(resonant, ground, 1);
        hidden.add_capacitor(resonant, ground, 1);
        reject(hidden.finalize(), {{visible, ground}});
    }
    {
        NetworkBuilder isolated;
        const auto n = isolated.add_node();
        reject(isolated.finalize(), {{n, n}});
        reject(isolated.finalize(), {{ground, ground}});
        const auto p = isolated.add_node(), q = isolated.add_node();
        reject(isolated.finalize(), {{p, ground}, {q, ground}});
    }
    {
        // Larger floating Laplacians need the exact loaded-connectivity guard;
        // roundoff-generated nonzero pivots do not establish a voltage reference.
        NetworkBuilder floating;
        std::vector<NodeId> nodes;
        for (std::size_t i = 0; i < 64; ++i) {
            nodes.push_back(floating.add_node());
        }
        std::mt19937_64 generator(421);
        std::uniform_real_distribution<double> exponent(-3, 3);
        for (std::size_t i = 1; i < nodes.size(); ++i) {
            floating.add_resistor(nodes[i - 1], nodes[i], std::pow(10., exponent(generator)));
        }
        for (std::size_t i = 0; i < 400; ++i) {
            const auto p = generator() % nodes.size(), n = generator() % nodes.size();
            if (p != n) {
                floating.add_resistor(nodes[p], nodes[n], std::pow(10., exponent(generator)));
            }
        }
        const auto network = floating.finalize();
        reject(network, {{nodes.front(), nodes.back()}});
        reject(network, {{nodes.front(), nodes.front()}});
        reject(network, {{nodes.front(), nodes.back()}, {nodes.back(), nodes.front()}});
        const auto referenced =
            extract_power_wave_s(network, omega_one_hz, {{nodes.front(), ground}}, {z1});
        near(referenced.scattering(0, 0),
             1.,
             "grounded boundary resolves large-network gauge",
             1e-9);
        diagnostics(referenced, {z1}, 65);
    }
    {
        // AC L branches are not ideal zero-voltage constraints: parallel 2 H
        // and 4 H have Z=j4/3 at omega=1. Hand reduction gives (25+j312)/313.
        NetworkBuilder inductors;
        const auto n = inductors.add_node();
        inductors.add_inductor(n, ground, 2);
        inductors.add_inductor(n, ground, 4);
        const auto result =
            extract_power_wave_s(inductors.finalize(), omega_one_hz, {{n, ground}}, {z1});
        matrix_near(result.scattering,
                    SMatrix{1, {Complex{25, 312} / 313.}},
                    "positive-frequency inductor cycle remains legal");
        diagnostics(result, {z1}, 4);
        unitary(result.scattering);
    }
}

void input_options_and_numeric_ranges() {
    NetworkBuilder builder;
    const auto n = builder.add_node();
    builder.add_resistor(n, ground, 2);
    const auto network = builder.finalize();
    const std::vector<Port> ports{{n, ground}};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double minimum = std::numeric_limits<double>::min();
    const double maximum = std::numeric_limits<double>::max();

    fails(
        [&] {
            (void)extract_power_wave_s(network, 1., {}, {});
        },
        Failure::invalid_input,
        "empty port list is invalid");
    for (const auto &references : {std::vector<Complex>{}, std::vector<Complex>{z1, z2}}) {
        fails(
            [&] {
                (void)extract_power_wave_s(network, 1., ports, references);
            },
            Failure::invalid_input,
            "reference count must exactly match port count");
    }
    for (const auto invalid : {Port{NodeId{2}, ground},
                               Port{ground, NodeId{2}},
                               Port{NodeId{2}, NodeId{2}},
                               Port{NodeId{std::numeric_limits<std::size_t>::max()}, ground}}) {
        fails(
            [&] {
                (void)extract_power_wave_s(network, 1., {invalid}, {z1});
            },
            Failure::invalid_input,
            "both terminal indices are validated against the snapshot");
    }
    for (double bad : {0., -0., -1., nan, inf, -inf, tiny, -tiny}) {
        fails(
            [&] {
                (void)extract_power_wave_s(network, bad, ports, {z1});
            },
            Failure::invalid_input,
            "frequency must be finite positive normal Hz");
        fails(
            [&] {
                (void)extract_power_wave_s(network, 1., ports, {Complex{bad, 0}});
            },
            Failure::invalid_input,
            "reference real part must be finite positive normal");
    }
    for (double bad : {nan, inf, -inf, tiny, -tiny}) {
        fails(
            [&] {
                (void)extract_power_wave_s(network, 1., ports, {Complex{4, bad}});
            },
            Failure::invalid_input,
            "reference imaginary part must be normal or exact zero");
    }
    for (double imaginary : {0., -0., 3., -3.}) {
        const auto result = extract_power_wave_s(network, 1., ports, {Complex{4, imaginary}});
        const Complex expected = imaginary == 0  ? Complex{-1. / 3., 0}
                                 : imaginary > 0 ? expected_two_ohms
                                                 : std::conj(expected_two_ohms);
        matrix_near(
            result.scattering, SMatrix{1, {expected}}, "both imaginary signs and signed zero");
    }

    using Member = double SolveOptions::*;
    const std::array<Member, 7> members{{&SolveOptions::voltage_scale_v,
                                         &SolveOptions::current_scale_a,
                                         &SolveOptions::pivot_relative_tolerance,
                                         &SolveOptions::backward_relative_tolerance,
                                         &SolveOptions::residual_relative_tolerance,
                                         &SolveOptions::kcl_absolute_tolerance_a,
                                         &SolveOptions::constraint_absolute_tolerance_v}};
    for (const auto member : members) {
        for (double bad : {0., -1., nan, inf, tiny}) {
            SolveOptions options;
            options.*member = bad;
            fails(
                [&] {
                    (void)extract_power_wave_s(network, 1., ports, {z1}, options);
                },
                Failure::invalid_input,
                "all numerical options use the existing strict domain");
        }
    }
    for (const auto member : {&SolveOptions::pivot_relative_tolerance,
                              &SolveOptions::backward_relative_tolerance,
                              &SolveOptions::residual_relative_tolerance}) {
        for (double bad : {1., 2.}) {
            SolveOptions options;
            options.*member = bad;
            fails(
                [&] {
                    (void)extract_power_wave_s(network, 1., ports, {z1}, options);
                },
                Failure::invalid_input,
                "relative numerical tolerance must remain below one");
        }
    }
    fails(
        [&] {
            (void)extract_power_wave_s(network, maximum, ports, {z1});
        },
        Failure::range_error,
        "normal input frequency can overflow angular-frequency arithmetic");
    {
        NetworkBuilder empty;
        fails(
            [&] {
                (void)extract_power_wave_s(
                    empty.finalize(), 1., {{ground, ground}}, {Complex{minimum, 0}});
            },
            Failure::range_error,
            "normal extreme references still obey checked intermediate range");
        // Both components are valid normal values. Bounded unit scaling permits
        // this fixture without ever needing an overflowing raw |Z| or Z*Z.
        const auto huge_short = extract_power_wave_s(
            empty.finalize(), 1., {{ground, ground}}, {Complex{maximum, maximum}});
        matrix_near(huge_short.scattering,
                    SMatrix{1, {Complex{0, 1}}},
                    "supported extreme complex reference preserves the exact short phase");
    }
    for (bool huge : {false, true}) {
        SolveOptions options;
        options.voltage_scale_v = huge ? maximum : minimum;
        options.current_scale_a = huge ? minimum : maximum;
        fails(
            [&] {
                (void)extract_power_wave_s(network, 1., ports, {z1}, options);
            },
            Failure::range_error,
            "unsupported unit scaling is never silently clipped");
    }
    {
        NetworkBuilder bad_resistor;
        const auto p = bad_resistor.add_node();
        bad_resistor.add_resistor(p, ground, maximum);
        fails(
            [&] {
                (void)extract_power_wave_s(bad_resistor.finalize(), 1., {{p, ground}}, {z1});
            },
            Failure::range_error,
            "subnormal reciprocal resistance is not converted to an open");
    }
    for (bool inductor : {false, true}) {
        for (bool overflow : {false, true}) {
            NetworkBuilder reactive;
            const auto p = reactive.add_node();
            if (inductor) {
                reactive.add_inductor(p, ground, overflow ? maximum : minimum);
            } else {
                reactive.add_capacitor(p, ground, overflow ? maximum : minimum);
            }
            fails(
                [&] {
                    (void)extract_power_wave_s(
                        reactive.finalize(), overflow ? 1. : minimum, {{p, ground}}, {z1});
                },
                Failure::range_error,
                "reactive stamp overflow and underflow remain explicit");
        }
    }
    NetworkBuilder ground_only;
    const auto smallest_frequency =
        extract_power_wave_s(ground_only.finalize(), minimum, {{ground, ground}}, {z1});
    matrix_near(smallest_frequency.scattering,
                SMatrix{1, {expected_short}},
                "minimum normal frequency is accepted when all arithmetic remains supported");
}

void residual_failures_and_transactional_results() {
    const auto network = make_rlc();
    const std::vector<Port> ports{{NodeId{1}, ground}, {NodeId{3}, ground}};
    auto retained = extract_power_wave_s(network, omega_one_hz, ports, complex_references);
    for (bool backward_only : {false, true}) {
        SolveOptions strict;
        strict.backward_relative_tolerance = backward_only ? 1e-30 : .5;
        strict.residual_relative_tolerance = backward_only ? .5 : 1e-30;
        strict.kcl_absolute_tolerance_a = backward_only ? 1. : 1e-30;
        strict.constraint_absolute_tolerance_v = backward_only ? 1. : 1e-30;
        fails(
            [&] {
                retained =
                    extract_power_wave_s(network, omega_one_hz, ports, complex_references, strict);
            },
            Failure::residual_failure,
            "physical and dimensionless backward gates each independently reject strict policy");
        matrix_near(
            retained.scattering, expected_rlc, "failed extraction does not publish partial S");
        diagnostics(retained, complex_references, 6);
    }
    matrix_near(extract_power_wave_s(network, omega_one_hz, ports, complex_references).scattering,
                expected_rlc,
                "failure cannot contaminate a repeated normal extraction");

    SolveOptions guarded;
    guarded.pivot_relative_tolerance = .9;
    fails(
        [&] {
            (void)extract_power_wave_s(network, omega_one_hz, ports, complex_references, guarded);
        },
        Failure::singular_or_ill_conditioned,
        "configured pivot gate is independently effective");
}

void augmented_budget_and_result_ownership() {
    {
        // 127 node voltages + 127 ideal branch currents + 2 port currents = 256.
        // The third port exceeds the total budget despite a valid bare snapshot.
        NetworkBuilder builder;
        std::vector<NodeId> nodes;
        for (std::size_t i = 0; i < 127; ++i) {
            nodes.push_back(builder.add_node());
            builder.add_voltage_source(nodes.back(), ground, {});
        }
        const auto network = builder.finalize();
        const auto edge = extract_power_wave_s(
            network, omega_one_hz, {{nodes[0], ground}, {nodes[1], ground}}, {z1, 9.});
        matrix_near(edge.scattering,
                    SMatrix{2, {expected_short, 0., 0., -1.}},
                    "augmented dimension exactly 256 is admitted");
        diagnostics(edge, {z1, 9.}, 256);
        fails(
            [&] {
                (void)extract_power_wave_s(
                    network,
                    omega_one_hz,
                    {{nodes[0], ground}, {nodes[1], ground}, {ground, ground}},
                    {z1, 9., 4.});
            },
            Failure::budget_exceeded,
            "257 includes internal branches and all port currents");
    }
    {
        NetworkBuilder empty;
        const auto network = empty.finalize();
        std::vector<Port> ports(257, Port{ground, ground});
        const std::vector<Complex> references(257, 4.);
        fails(
            [&] {
                (void)extract_power_wave_s(network, omega_one_hz, ports, references);
            },
            Failure::budget_exceeded,
            "ports alone cannot exceed the augmented allocation budget");
    }
    {
        NetworkBuilder maximum_bare;
        for (std::size_t i = 0; i < 256; ++i) {
            const auto n = maximum_bare.add_node();
            maximum_bare.add_resistor(n, ground, 1);
        }
        fails(
            [&] {
                (void)extract_power_wave_s(
                    maximum_bare.finalize(), omega_one_hz, {{ground, ground}}, {4.});
            },
            Failure::budget_exceeded,
            "maximum bare dimension leaves no room for a port");
    }

    static_assert(
        std::is_same<decltype(std::declval<PowerWaveSResult>().scattering), SMatrix>::value,
        "the new layer returns the existing public SMatrix type");
    static_assert(std::is_copy_constructible<PowerWaveSResult>::value,
                  "extracted results own their data and are copyable");
    NetworkBuilder builder;
    const auto n = builder.add_node();
    builder.add_resistor(n, ground, 2);
    const auto snapshot = builder.finalize();
    std::vector<Port> ports{{n, ground}};
    std::vector<Complex> references{z1};
    const auto result = extract_power_wave_s(snapshot, omega_one_hz, ports, references);
    references[0] = z2;
    ports[0] = {ground, n};
    builder.add_resistor(n, ground, 2);
    (void)builder.add_node();
    require(snapshot.node_count() == 2 && snapshot.element_count() == 1,
            "builder changes do not alter extracted snapshot contents");
    diagnostics(result, {z1}, 2);
    matrix_near(result.scattering,
                SMatrix{1, {expected_two_ohms}},
                "input-container edits cannot alter the owning result");
    auto copy = result;
    copy.scattering(0, 0) = 99.;
    copy.references_ohm[0] = 100.;
    copy.column_diagnostics[0].max_kcl_residual_a = 123.;
    matrix_near(
        result.scattering, SMatrix{1, {expected_two_ohms}}, "copied result storage is independent");
    diagnostics(result, {z1}, 2);
    for (int repeat = 0; repeat < 4; ++repeat) {
        matrix_near(extract_power_wave_s(snapshot, omega_one_hz, {{n, ground}}, {z1}).scattering,
                    SMatrix{1, {expected_two_ohms}},
                    "repeated extraction is const and deterministic");
    }
    const auto escaped = [] {
        NetworkBuilder local;
        const auto p = local.add_node();
        local.add_resistor(p, ground, 2);
        return extract_power_wave_s(local.finalize(), omega_one_hz, {{p, ground}}, {z1});
    }();
    diagnostics(escaped, {z1}, 2);
    matrix_near(escaped.scattering,
                SMatrix{1, {expected_two_ohms}},
                "result outlives builder, snapshot, ports and reference temporaries");
}

void existing_power_wave_helpers_and_connections() {
    NetworkBuilder duplicate;
    const auto n = duplicate.add_node();
    const auto through = extract_power_wave_s(
        duplicate.finalize(), omega_one_hz, {{n, ground}, {n, ground}}, complex_references);
    const auto normalized = rfmodel::renormalize_power_waves(
        SMatrix{2, {0., 1., 1., 0.}}, {4., 4.}, complex_references);
    matrix_near(
        normalized.scattering, expected_through, "existing renormalization exact through oracle");
    matrix_near(through.scattering,
                normalized.scattering,
                "MNA through matches wave-coordinate conversion");
    near(rfmodel::renormalize_power_waves(SMatrix{1, {-1.}}, {4.}, {z1}).scattering(0, 0),
         expected_short,
         "ideal short compatibility needs no finite Y");
    bool short_has_no_finite_y = false;
    try {
        (void)rfmodel::s_to_y(SMatrix{1, {-1.}}, std::vector<Complex>{4.});
    } catch (const std::domain_error &) {
        short_has_no_finite_y = true;
    }
    require(short_has_no_finite_y, "ideal short is not representable by finite admittance");
    near(rfmodel::admittance_to_power_wave_s(SMatrix{1, {0.}}, {z1})(0, 0),
         1.,
         "existing helper retains its valid zero-Y case");
    const auto finite_series = rfmodel::admittance_to_power_wave_s(
        SMatrix{2, {1. / 6., -1. / 6., -1. / 6., 1. / 6.}}, {4., 9.});
    matrix_near(finite_series,
                SMatrix{2, {11. / 19., 12. / 19., 12. / 19., 1. / 19.}},
                "existing helper retains its valid singular finite-Y case");
    const Complex series{2, 3}, shunt{0, .5};
    const auto finite_rlc = rfmodel::admittance_to_power_wave_s(
        SMatrix{2, {1. / series, -1. / series, -1. / series, 1. / series + shunt}},
        complex_references);
    matrix_near(finite_rlc, expected_rlc, "existing finite-Y formula agrees with exact RLC oracle");
    matrix_near(extract_power_wave_s(make_rlc(),
                                     omega_one_hz,
                                     {{NodeId{1}, ground}, {NodeId{3}, ground}},
                                     complex_references)
                    .scattering,
                finite_rlc,
                "MNA and finite-Y paths agree where both are defined");

    // The physical load is R=5 in series with C=1/2: ZL=5-j2. Give
    // the separate load an independent reference different from both through refs.
    NetworkBuilder load;
    const auto input = load.add_node(), capacitor = load.add_node();
    load.add_resistor(input, capacitor, 5);
    load.add_capacitor(capacitor, ground, .5);
    const auto load_network = load.finalize();
    const Complex load_reference{16, 1};
    const auto extracted_load =
        extract_power_wave_s(load_network, omega_one_hz, {{input, ground}}, {load_reference});
    const auto direct = extract_power_wave_s(load_network, omega_one_hz, {{input, ground}}, {z1});
    matrix_near(direct.scattering,
                SMatrix{1, {Complex{5, 4} / 41.}},
                "direct physical RC load exact oracle");

    rfmodel::LinearNetwork assembled;
    const auto through_offset =
        assembled.add_power_waves(through.scattering, through.references_ohm);
    const auto load_offset =
        assembled.add_power_waves(extracted_load.scattering, extracted_load.references_ohm);
    assembled.connect(through_offset + 1, load_offset);
    const auto connected = assembled.external_s({through_offset});
    matrix_near(
        connected, direct.scattering, "existing physical connect composes extracted blocks");

    // terminate is a DUT-coordinate boundary, a=Gamma*b. Its exact coefficient
    // is -2/7 here, distinct from this load's own scattering coefficient.
    rfmodel::LinearNetwork terminated;
    const auto offset = terminated.add_power_waves(through.scattering, through.references_ohm);
    terminated.terminate(offset + 1, -2. / 7.);
    matrix_near(terminated.external_s({offset}),
                direct.scattering,
                "correct complex-reference termination agrees with physical connect");
    const auto own_load = extract_power_wave_s(load_network, omega_one_hz, {{input, ground}}, {z2});
    require(std::abs(own_load.scattering(0, 0) + 2. / 7.) > .1,
            "load-owned S is deliberately not the DUT boundary reflection");
    rfmodel::LinearNetwork wrong_boundary;
    const auto wrong_offset =
        wrong_boundary.add_power_waves(through.scattering, through.references_ohm);
    wrong_boundary.terminate(wrong_offset + 1, own_load.scattering(0, 0));
    require(std::abs(wrong_boundary.external_s({wrong_offset})(0, 0) - direct.scattering(0, 0)) >
                .1,
            "mutation sentinel detects confusing load S with termination reflection");
}
} // namespace

int main() {
    try {
        open_short_and_one_port_oracles();
        duplicate_reversed_dependent_and_ordered_ports();
        resistor_rlc_and_physical_scaling();
        sources_are_incremental_and_unchanged();
        nonunique_loaded_states_and_legal_inductor_cycles();
        input_options_and_numeric_ranges();
        residual_failures_and_transactional_results();
        augmented_budget_and_result_ownership();
        existing_power_wave_helpers_and_connections();
        std::cout << "PASS: MNA power-wave S (" << checks << " always-active checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: MNA power-wave S after " << checks << " checks: " << error.what()
                  << '\n';
        return 1;
    }
}
