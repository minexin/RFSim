#include "rfmodel/mna/linear_network.hpp"

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
std::size_t checks = 0;
constexpr double pi = 3.1415926535897932384626433832795;
const double omega_one_hz = 1.0 / (2.0 * pi);

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

template <class F> void fails(F action, Failure expected, const char *message) {
    ++checks;
    try {
        action();
    } catch (const SolveError &error) {
        require(error.failure() == expected, message);
        require(std::string(error.what()).size() > 0, "structured failure has an explanation");
        return;
    }
    throw std::runtime_error(message);
}

template <class Scalar>
void diagnostics_and_ids(const Solution<Scalar> &result,
                         std::size_t nodes,
                         std::size_t elements,
                         const SolveOptions &options = {}) {
    require(result.node_voltage.size() == nodes, "stable node result count includes ground");
    require(result.elements.size() == elements, "stable element result count");
    require(result.node_voltage[0] == Scalar{}, "ground is exactly zero");
    for (std::size_t i = 0; i < elements; ++i) {
        require(result.elements[i].id.value == i, "stable element result ID");
    }
    const auto &d = result.diagnostics;
    require(std::isfinite(d.max_scaled_residual) && d.max_scaled_residual >= 0 &&
                d.max_scaled_residual <= options.backward_relative_tolerance,
            "measured admitted backward residual");
    require(std::isfinite(d.max_kcl_residual_a) && d.max_kcl_residual_a >= 0,
            "finite measured KCL residual");
    require(std::isfinite(d.max_constraint_residual_v) && d.max_constraint_residual_v >= 0,
            "finite measured voltage residual");
    require(std::isfinite(d.min_scaled_pivot) &&
                d.min_scaled_pivot > options.pivot_relative_tolerance,
            "measured admitted scaled pivot");
    require(std::isfinite(d.effective_pivot_tolerance) && d.effective_pivot_tolerance >= 0,
            "reported effective pivot floor is finite");
    if (nodes > 1) {
        require(d.effective_pivot_tolerance >= options.pivot_relative_tolerance &&
                    d.min_scaled_pivot > d.effective_pivot_tolerance,
                "admitted pivot exceeds requested tolerance and roundoff floor");
    }
    require(d.refinement_steps <= 3, "iterative refinement obeys its fixed budget");
    require(d.min_equilibration_exponent <= d.max_equilibration_exponent,
            "ordered equilibration exponent diagnostics");
}

template <class Scalar> void conserved_power(const Solution<Scalar> &result, const char *message) {
    Complex total{}, scale{};
    for (const auto &e : result.elements) {
        const Complex v = e.voltage;
        const Complex i = e.current;
        near(e.power, v * std::conj(i), "element power obeys passive RMS convention");
        total += e.power;
        scale += std::abs(e.power);
    }
    require(std::abs(total) <= 1e-12 + 1e-10 * scale.real(), message);
}

void divider_and_floating_source() {
    // Independent hand calculation: I=12/(2000+1000), Vout=1000*I.
    NetworkBuilder builder;
    const auto supply = builder.add_node(), output = builder.add_node();
    const auto source = builder.add_voltage_source(supply, ground, {12, {3, 4}});
    const auto upper = builder.add_resistor(supply, output, 2000);
    const auto lower = builder.add_resistor(output, ground, 1000);
    const auto network = builder.finalize();
    const auto dc = solve_dc(network);
    diagnostics_and_ids(dc, 3, 3);
    near(dc.node_voltage[supply.value], 12, "divider supply DC");
    near(dc.node_voltage[output.value], 4, "divider output DC");
    near(dc.elements[source.value].current, -1.0 / 250, "delivering DC V source has negative I");
    near(dc.elements[upper.value].voltage, 8, "upper resistor terminal voltage");
    near(dc.elements[upper.value].current, .004, "upper resistor current");
    near(dc.elements[upper.value].power, .032, "upper resistor DC power");
    near(dc.elements[lower.value].power, .016, "lower resistor DC power");
    near(dc.elements[source.value].power, -.048, "source DC power");
    near(dc.elements[upper.value].current, dc.elements[lower.value].current, "divider output KCL");
    conserved_power(dc, "divider DC total absorbed power is zero");

    const auto ac = solve_ac(network, 1234);
    diagnostics_and_ids(ac, 3, 3);
    near(ac.node_voltage[supply.value], {3, 4}, "DC excitation does not leak into AC");
    near(ac.node_voltage[output.value], {1, 4.0 / 3}, "complex resistive divider");
    near(ac.elements[source.value].current, {-0.001, -4.0 / 3000}, "AC V source sign");
    near(ac.elements[source.value].power, -25.0 / 3000, "RMS source power has no factor 1/2");
    conserved_power(ac, "divider AC total absorbed complex power is zero");

    // Independent exact oracle: v1-v2=6, v1/2+v2/4=0 -> (2,-4,-1).
    NetworkBuilder floating;
    const auto a = floating.add_node(), b = floating.add_node();
    const auto r1 = floating.add_resistor(a, ground, 2);
    const auto v = floating.add_voltage_source(a, b, {6, {6, 0}});
    const auto r2 = floating.add_resistor(b, ground, 4);
    const auto f = solve_dc(floating.finalize());
    near(f.node_voltage[a.value], 2, "floating ideal V source positive node");
    near(f.node_voltage[b.value], -4, "floating ideal V source negative node");
    near(f.elements[v.value].voltage, 6, "floating ideal source voltage");
    near(f.elements[v.value].current, -1, "floating ideal source current");
    near(f.elements[r1.value].power, 2, "floating circuit first load power");
    near(f.elements[r2.value].power, 4, "floating circuit second load power");
    near(f.elements[v.value].power, -6, "floating circuit source power");
    conserved_power(f, "floating circuit power balance");
    conserved_power(solve_ac(floating.finalize(), omega_one_hz), "floating AC power balance");
}

void reactive_oracles_and_dc_limits() {
    // Exact SymPy oracle in linear-mna-design/oracle-results.txt, independent of production stamps.
    NetworkBuilder rl;
    const auto s = rl.add_node(), l = rl.add_node();
    const auto source = rl.add_voltage_source(s, ground, {10, {10, 0}});
    const auto resistor = rl.add_resistor(s, l, 2);
    const auto inductor = rl.add_inductor(l, ground, 3);
    const auto net = rl.finalize();
    const auto ac = solve_ac(net, omega_one_hz);
    const Complex expected_i{20.0 / 13, -30.0 / 13};
    diagnostics_and_ids(ac, 3, 3);
    near(ac.node_voltage[l.value], {90.0 / 13, 60.0 / 13}, "RL exact inductor voltage");
    near(ac.elements[inductor.value].current, expected_i, "RL exact lagging current");
    near(ac.elements[source.value].current, -expected_i, "RL source branch sign");
    near(ac.elements[resistor.value].power, 200.0 / 13, "RL absorbed real power");
    near(ac.elements[inductor.value].power, {0, 300.0 / 13}, "L positive reactive power");
    near(ac.elements[source.value].power, {-200.0 / 13, -300.0 / 13}, "RL source power");
    near(ac.elements[resistor.value].current, ac.elements[inductor.value].current, "RL KCL");
    conserved_power(ac, "RL AC conservation");
    const auto dc = solve_dc(net);
    near(dc.node_voltage[l.value], 0, "DC L is exact short");
    near(dc.elements[inductor.value].current, 5, "DC L has retained branch current");
    near(dc.elements[inductor.value].voltage, 0, "DC L terminal voltage zero");
    near(dc.elements[inductor.value].power, 0, "DC L power zero");
    near(dc.elements[source.value].current, -5, "DC RL source branch current");
    conserved_power(dc, "DC RL conservation");

    NetworkBuilder rlc;
    const auto a = rlc.add_node(), b = rlc.add_node(), c = rlc.add_node();
    const auto vs = rlc.add_voltage_source(a, ground, {10, {10, 0}});
    const auto r = rlc.add_resistor(a, b, 2);
    const auto il = rlc.add_inductor(b, c, 3);
    const auto cap = rlc.add_capacitor(c, ground, .5);
    const auto rlcnet = rlc.finalize();
    const auto q = solve_ac(rlcnet, omega_one_hz);
    diagnostics_and_ids(q, 4, 4);
    near(q.node_voltage[b.value], {2, 4}, "RLC node before L oracle");
    near(q.node_voltage[c.value], {-4, -8}, "RLC capacitor voltage oracle");
    for (const auto id : {r, il, cap}) {
        near(q.elements[id.value].current, {4, -2}, "RLC series current oracle");
    }
    near(q.elements[vs.value].current, {-4, 2}, "RLC source current oracle");
    near(q.elements[r.value].power, 40, "RLC resistor power");
    near(q.elements[il.value].power, {0, 60}, "RLC L absorbed reactive power");
    near(q.elements[cap.value].power, {0, -40}, "C negative reactive power");
    near(q.elements[vs.value].power, {-40, -20}, "RLC source complex power");
    conserved_power(q, "RLC AC complex conservation");
    const auto d = solve_dc(rlcnet);
    for (const auto id : {a, b, c}) {
        near(d.node_voltage[id.value], 10, "DC RLC capacitor open and inductor short");
    }
    for (const auto &e : d.elements) {
        near(e.current, 0, "DC open-series-path current zero");
        near(e.power, 0, "DC open-series-path power zero");
    }

    // Same independent oracle, with node allocation, element order, and every orientation reversed.
    NetworkBuilder reversed;
    const auto rc = reversed.add_node(), rb = reversed.add_node(), ra = reversed.add_node();
    const auto reversed_c = reversed.add_capacitor(ground, rc, .5);
    const auto reversed_l = reversed.add_inductor(rc, rb, 3);
    const auto reversed_r = reversed.add_resistor(rb, ra, 2);
    const auto reversed_v = reversed.add_voltage_source(ground, ra, {-10, {-10, 0}});
    const auto rq = solve_ac(reversed.finalize(), omega_one_hz);
    near(rq.node_voltage[ra.value], 10, "reordered RLC supply voltage");
    near(rq.node_voltage[rb.value], {2, 4}, "reordered RLC node before L");
    near(rq.node_voltage[rc.value], {-4, -8}, "reordered RLC capacitor node");
    for (const auto id : {reversed_r, reversed_l, reversed_c}) {
        near(rq.elements[id.value].current, {-4, 2}, "R/L/C reversal flips oriented current");
    }
    near(
        rq.elements[reversed_v.value].current, {4, -2}, "reordered reversed source branch current");
    near(rq.elements[reversed_l.value].power, {0, 60}, "L reversal preserves absorbed power");
    near(rq.elements[reversed_c.value].power, {0, -40}, "C reversal preserves absorbed power");
    conserved_power(rq, "reordered RLC conservation");
}

void current_polarity_and_ac_without_dc() {
    for (bool reverse : {false, true}) {
        NetworkBuilder builder;
        const auto n = builder.add_node();
        const auto current =
            builder.add_current_source(reverse ? ground : n, reverse ? n : ground, {2, {1, 2}});
        const auto resistor = builder.add_resistor(n, ground, 3);
        const auto network = builder.finalize();
        const auto dc = solve_dc(network);
        const double sign = reverse ? 1 : -1;
        near(dc.node_voltage[n.value], sign * 6, "current source p-to-n DC injection polarity");
        near(dc.elements[current.value].current, 2, "current source output preserves orientation");
        near(dc.elements[current.value].voltage, -6, "current source passive terminal voltage");
        near(dc.elements[current.value].power, -12, "current source supplies resistor");
        near(dc.elements[resistor.value].power, 12, "current source load power");
        conserved_power(dc, "current source DC conservation");
        const auto ac = solve_ac(network, omega_one_hz);
        near(ac.node_voltage[n.value], sign * Complex{3, 6}, "current source complex injection");
        near(ac.elements[current.value].power, -15, "AC current-source RMS real power");
        conserved_power(ac, "current source AC conservation");
    }

    // At DC this node has no reference admittance; positive-frequency C closes the path.
    NetworkBuilder capacitor;
    const auto n = capacitor.add_node();
    const auto cap = capacitor.add_capacitor(n, ground, .5);
    const auto current = capacitor.add_current_source(ground, n, {2, {2, 0}});
    const auto network = capacitor.finalize();
    fails(
        [&] {
            (void)solve_dc(network);
        },
        Failure::singular_or_ill_conditioned,
        "DC capacitor-only node must not receive invented zero voltage");
    const auto ac = solve_ac(network, omega_one_hz);
    near(ac.node_voltage[n.value], {0, -4}, "AC succeeds independently after singular DC");
    near(ac.elements[cap.value].current, 2, "C current leads voltage");
    near(ac.elements[cap.value].power, {0, -8}, "C absorbed complex power");
    near(ac.elements[current.value].power, {0, 8}, "current-source reactive exchange");
    conserved_power(ac, "capacitor-current-source conservation");
}

void singular_topologies() {
    const auto singular_dc = [](const Network &net) {
        fails(
            [&] {
                (void)solve_dc(net);
            },
            Failure::singular_or_ill_conditioned,
            "singular DC topology must fail without a partial solution");
    };
    const auto singular_ac = [](const Network &net) {
        fails(
            [&] {
                (void)solve_ac(net, omega_one_hz);
            },
            Failure::singular_or_ill_conditioned,
            "singular AC topology must fail without a partial solution");
    };
    for (double second : {1.0, 2.0}) {
        NetworkBuilder b;
        const auto n = b.add_node();
        b.add_voltage_source(n, ground, {1, {1, 0}});
        b.add_voltage_source(n, ground, {second, {second, 0}});
        b.add_resistor(n, ground, 3);
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
    for (double closing : {3.0, 4.0}) {
        NetworkBuilder b;
        const auto a = b.add_node(), n = b.add_node();
        b.add_voltage_source(a, n, {1, {1, 0}});
        b.add_voltage_source(n, ground, {2, {2, 0}});
        b.add_voltage_source(a, ground, {closing, {closing, 0}});
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
    NetworkBuilder inductors;
    const auto n = inductors.add_node();
    const auto l1 = inductors.add_inductor(n, ground, 2);
    const auto l2 = inductors.add_inductor(n, ground, 4);
    inductors.add_current_source(ground, n, {3, {3, 0}});
    singular_dc(inductors.finalize());
    const auto ac = solve_ac(inductors.finalize(), omega_one_hz);
    near(ac.node_voltage[n.value], {0, 4}, "positive-frequency inductor loop is legal");
    near(ac.elements[l1.value].current, 2, "parallel L current first branch");
    near(ac.elements[l2.value].current, 1, "parallel L current second branch");
    conserved_power(ac, "parallel L AC conservation");

    for (double voltage : {0.0, 1.0}) {
        NetworkBuilder b;
        const auto a = b.add_node(), c = b.add_node();
        b.add_inductor(a, c, 2);
        b.add_inductor(c, ground, 3);
        b.add_voltage_source(a, ground, {voltage, {voltage, 0}});
        singular_dc(b.finalize());
        // The same V/L loop is well-defined at positive frequency.
        (void)solve_ac(b.finalize(), omega_one_hz);
    }
    {
        NetworkBuilder b;
        const auto a = b.add_node(), c = b.add_node();
        b.add_inductor(a, c, 2);
        b.add_inductor(c, ground, 3);
        b.add_inductor(a, ground, 4);
        singular_dc(b.finalize());
        (void)solve_ac(b.finalize(), omega_one_hz);
    }
    for (double voltage : {0.0, 2.0}) {
        NetworkBuilder b;
        const auto a = b.add_node();
        const auto v = b.add_voltage_source(a, ground, {voltage, {voltage, 0}});
        const auto l = b.add_inductor(a, ground, 4);
        singular_dc(b.finalize());
        const auto result = solve_ac(b.finalize(), omega_one_hz);
        near(result.node_voltage[a.value], voltage, "AC V/L parallel constraint is legal");
        near(result.elements[l.value].current, {0, -voltage / 4}, "AC V/L unique inductor current");
        near(result.elements[v.value].current, {0, voltage / 4}, "AC V/L unique source current");
        conserved_power(result, "AC V/L parallel power conservation");
    }
    {
        // A grounded current source injects current but cannot fix common-mode voltage.
        NetworkBuilder b;
        const auto a = b.add_node(), c = b.add_node();
        b.add_resistor(a, c, 1);
        b.add_current_source(ground, a, {1, {1, 0}});
        b.add_current_source(c, ground, {1, {1, 0}});
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
    {
        // Connectivity is an exact prerequisite; rounded LU must not admit a floating Laplacian.
        NetworkBuilder b;
        std::vector<NodeId> nodes;
        for (std::size_t i = 0; i < 256; ++i) {
            nodes.push_back(b.add_node());
        }
        std::mt19937_64 generator(421);
        std::uniform_real_distribution<double> exponent(-3, 3);
        for (std::size_t i = 1; i < nodes.size(); ++i) {
            b.add_resistor(nodes[i - 1], nodes[i], std::pow(10.0, exponent(generator)));
        }
        std::size_t count = 255;
        while (count < 4096) {
            const auto p = static_cast<std::size_t>(generator() % 256);
            const auto n = static_cast<std::size_t>(generator() % 256);
            if (p != n) {
                b.add_resistor(nodes[p], nodes[n], std::pow(10.0, exponent(generator)));
                ++count;
            }
        }
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
    {
        NetworkBuilder b;
        (void)b.add_node();
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
    {
        NetworkBuilder b;
        const auto a = b.add_node(), c = b.add_node();
        b.add_resistor(a, c, 2);
        b.add_current_source(a, c, {1, {1, 0}});
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
    {
        // Each grounded L=1/C=1 pair cancels at omega=1. The exact nonzero null state
        // is every node voltage=1 and every L current=-j; all resistor currents vanish.
        // Advancing one persistent RNG reproduces the rounded-LU admission regression.
        std::mt19937_64 generator(421);
        std::uniform_real_distribution<double> exponent(-3, 3);
        for (int sample = 0; sample <= 8; ++sample) {
            NetworkBuilder b;
            std::vector<NodeId> nodes;
            for (std::size_t k = 0; k < 128; ++k) {
                nodes.push_back(b.add_node());
                b.add_capacitor(nodes.back(), ground, 1);
                b.add_inductor(nodes.back(), ground, 1);
            }
            for (std::size_t k = 1; k < 128; ++k) {
                b.add_resistor(nodes[k - 1], nodes[k], std::pow(10.0, exponent(generator)));
            }
            for (std::size_t count = 383; count < 4096; ++count) {
                const auto p = static_cast<std::size_t>(generator() % 128);
                auto n = static_cast<std::size_t>(generator() % 128);
                if (p == n) {
                    n = (n + 1) % 128;
                }
                b.add_resistor(nodes[p], nodes[n], std::pow(10.0, exponent(generator)));
            }
            if (sample == 8) {
                singular_ac(b.finalize());
            }
        }
    }
    for (double excitation : {0.0, 1.0}) {
        NetworkBuilder b;
        const auto a = b.add_node();
        b.add_capacitor(a, ground, 1);
        b.add_current_source(a, ground, {excitation, {excitation, 0}});
        singular_dc(b.finalize());
        b.add_inductor(a, ground, 1);
        singular_ac(b.finalize()); // Ideal parallel resonance at omega=1.
    }
    {
        NetworkBuilder b;
        const auto a = b.add_node();
        b.add_current_source(a, ground, {0, {0, 0}});
        singular_dc(b.finalize());
        singular_ac(b.finalize());
    }
}

void options_and_near_singular() {
    NetworkBuilder b;
    const auto a = b.add_node(), c = b.add_node();
    b.add_voltage_source(a, ground, {7, {7, 3}});
    b.add_resistor(a, c, 3);
    b.add_resistor(c, ground, 7);
    const auto network = b.finalize();
    using Member = double SolveOptions::*;
    const std::array<Member, 7> members{{&SolveOptions::voltage_scale_v,
                                         &SolveOptions::current_scale_a,
                                         &SolveOptions::pivot_relative_tolerance,
                                         &SolveOptions::backward_relative_tolerance,
                                         &SolveOptions::residual_relative_tolerance,
                                         &SolveOptions::kcl_absolute_tolerance_a,
                                         &SolveOptions::constraint_absolute_tolerance_v}};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double tiny = std::numeric_limits<double>::denorm_min();
    for (const auto member : members) {
        for (double bad : {0.0, -1.0, nan, inf, tiny}) {
            SolveOptions options;
            options.*member = bad;
            fails(
                [&] {
                    (void)solve_dc(network, options);
                },
                Failure::invalid_input,
                "invalid DC numerical policy must fail");
            fails(
                [&] {
                    (void)solve_ac(network, 1, options);
                },
                Failure::invalid_input,
                "invalid AC numerical policy must fail");
        }
    }
    for (const auto member : {&SolveOptions::pivot_relative_tolerance,
                              &SolveOptions::backward_relative_tolerance,
                              &SolveOptions::residual_relative_tolerance}) {
        for (double bad : {1.0, 2.0}) {
            SolveOptions options;
            options.*member = bad;
            fails(
                [&] {
                    (void)solve_dc(network, options);
                },
                Failure::invalid_input,
                "relative tolerance at least one is invalid");
        }
    }
    for (double frequency : {0.0, -1.0, nan, inf, tiny}) {
        fails(
            [&] {
                (void)solve_ac(network, frequency);
            },
            Failure::invalid_input,
            "AC frequency requires finite positive normal value");
    }
    for (double voltage_scale : {1e-6, 1.0, 1e6}) {
        for (double current_scale : {1e-9, 1e-3, 1e3}) {
            SolveOptions options;
            options.voltage_scale_v = voltage_scale;
            options.current_scale_a = current_scale;
            const auto dc = solve_dc(network, options);
            const auto ac = solve_ac(network, omega_one_hz, options);
            near(dc.node_voltage[c.value], 4.9, "unit scale choices preserve DC solution");
            near(ac.node_voltage[c.value], {4.9, 2.1}, "unit scale choices preserve AC solution");
            diagnostics_and_ids(dc, 3, 3, options);
            diagnostics_and_ids(ac, 3, 3, options);
        }
    }
    SolveOptions strict;
    strict.backward_relative_tolerance = 1e-30;
    strict.residual_relative_tolerance = 1e-30;
    strict.kcl_absolute_tolerance_a = 1e-30;
    strict.constraint_absolute_tolerance_v = 1e-30;
    fails(
        [&] {
            (void)solve_ac(network, omega_one_hz, strict);
        },
        Failure::residual_failure,
        "unattainable residual policy must return structured failure");

    SolveOptions backward_only;
    backward_only.backward_relative_tolerance = 1e-30;
    // Physical gates are permissive here, so only dimensionless backward error can reject.
    backward_only.residual_relative_tolerance = .5;
    backward_only.kcl_absolute_tolerance_a = 1;
    backward_only.constraint_absolute_tolerance_v = 1;
    NetworkBuilder mixed;
    const auto ms = mixed.add_node(), ma = mixed.add_node(), mb = mixed.add_node();
    mixed.add_voltage_source(ms, ground, {7.3, {7.3, 4.1}});
    mixed.add_resistor(ms, ma, 1.1);
    mixed.add_resistor(ma, mb, 2.3);
    mixed.add_resistor(mb, ground, 3.7);
    mixed.add_capacitor(ma, ground, .31);
    mixed.add_inductor(ma, mb, .17);
    mixed.add_capacitor(mb, ground, .23);
    mixed.add_current_source(mb, ground, {.37, {.37, .19}});
    const auto mixed_network = mixed.finalize();
    conserved_power(solve_ac(mixed_network, 1), "mixed circuit normal policy succeeds");
    fails(
        [&] {
            (void)solve_ac(mixed_network, 1, backward_only);
        },
        Failure::residual_failure,
        "backward-only admission gate is independently effective");

    SolveOptions physical_only;
    physical_only.backward_relative_tolerance = .5;
    physical_only.residual_relative_tolerance = 1e-30;
    physical_only.kcl_absolute_tolerance_a = 1e-30;
    physical_only.constraint_absolute_tolerance_v = 1e-30;
    fails(
        [&] {
            (void)solve_ac(network, omega_one_hz, physical_only);
        },
        Failure::residual_failure,
        "physical-only admission gate is independently effective");

    // An unrelated large state can inflate a normwise scale, but never excuses a bad local row.
    const auto unrelated = b.add_node();
    b.add_voltage_source(unrelated, ground, {1e20, {1e20, 0}});
    b.add_resistor(unrelated, ground, 1);
    const auto multiscale = b.finalize();
    const auto regular = solve_ac(multiscale, omega_one_hz);
    near(
        regular.node_voltage[c.value], {4.9, 2.1}, "unrelated huge state preserves small solution");
    near(regular.node_voltage[unrelated.value], 1e20, "unrelated huge state physical value");
    fails(
        [&] {
            (void)solve_ac(multiscale, omega_one_hz, physical_only);
        },
        Failure::residual_failure,
        "huge unrelated state cannot bypass original-unit per-row gate");

    const auto weak_reference = [](double shunt) {
        NetworkBuilder weak;
        const auto x = weak.add_node(), y = weak.add_node();
        weak.add_resistor(x, y, 1);
        weak.add_resistor(x, ground, shunt);
        weak.add_resistor(y, ground, shunt);
        weak.add_current_source(ground, x, {1, {1, 0}});
        weak.add_current_source(ground, y, {1, {1, 0}});
        return weak.finalize();
    };
    // Symmetry independently gives Vx=Vy=Rshunt. Do not infer forward accuracy from residuals.
    const auto admitted = solve_dc(weak_reference(1e5));
    near(admitted.node_voltage[1], 1e5, "moderately weak reference solution", 1e-10);
    near(admitted.node_voltage[2], 1e5, "moderately weak reference symmetry", 1e-10);
    const auto ill = weak_reference(1e16);
    fails(
        [&] {
            (void)solve_dc(ill);
        },
        Failure::singular_or_ill_conditioned,
        "numerically lost reference must not be silently regularized");
    SolveOptions guarded;
    guarded.pivot_relative_tolerance = 1e-6;
    const auto near_singular = weak_reference(1e9);
    fails(
        [&] {
            (void)solve_dc(near_singular, guarded);
        },
        Failure::singular_or_ill_conditioned,
        "configured scaled-pivot admission gate is effective");
}

void input_domain_and_arithmetic_range() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double tiny = std::numeric_limits<double>::denorm_min();
    NetworkBuilder b;
    const auto n = b.add_node();
    for (double bad : {0.0, -1.0, nan, inf, tiny}) {
        fails(
            [&] {
                (void)b.add_resistor(n, ground, bad);
            },
            Failure::invalid_input,
            "invalid R");
        fails(
            [&] {
                (void)b.add_capacitor(n, ground, bad);
            },
            Failure::invalid_input,
            "invalid C");
        fails(
            [&] {
                (void)b.add_inductor(n, ground, bad);
            },
            Failure::invalid_input,
            "invalid L");
    }
    for (const auto &nodes : {std::pair<NodeId, NodeId>{n, n},
                              {ground, ground},
                              {NodeId{2}, ground},
                              {ground, NodeId{2}},
                              {NodeId{std::numeric_limits<std::size_t>::max()}, ground}}) {
        fails(
            [&] {
                (void)b.add_resistor(nodes.first, nodes.second, 1);
            },
            Failure::invalid_input,
            "R rejects absent nodes and self loops");
        fails(
            [&] {
                (void)b.add_capacitor(nodes.first, nodes.second, 1);
            },
            Failure::invalid_input,
            "C rejects absent nodes and self loops");
        fails(
            [&] {
                (void)b.add_inductor(nodes.first, nodes.second, 1);
            },
            Failure::invalid_input,
            "L rejects absent nodes and self loops");
        fails(
            [&] {
                (void)b.add_voltage_source(nodes.first, nodes.second, {});
            },
            Failure::invalid_input,
            "V rejects absent nodes and self loops");
        fails(
            [&] {
                (void)b.add_current_source(nodes.first, nodes.second, {});
            },
            Failure::invalid_input,
            "I rejects absent nodes and self loops");
    }
    for (double bad : {nan, inf, tiny}) {
        for (const auto source :
             {SourceValue{bad, {}}, SourceValue{0, {bad, 0}}, SourceValue{0, {0, bad}}}) {
            fails(
                [&] {
                    (void)b.add_voltage_source(n, ground, source);
                },
                Failure::invalid_input,
                "V rejects invalid DC and either AC component");
            fails(
                [&] {
                    (void)b.add_current_source(n, ground, source);
                },
                Failure::invalid_input,
                "I rejects invalid DC and either AC component");
        }
    }
    require(b.finalize().element_count() == 0, "failed insertion never mutates builder");
    require(b.add_resistor(n, ground, 1).value == 0, "failed insertion does not consume IDs");

    const auto range = [](const Network &net, bool ac, double hz = omega_one_hz) {
        fails(
            [&] {
                if (ac) {
                    (void)solve_ac(net, hz);
                } else {
                    (void)solve_dc(net);
                }
            },
            Failure::range_error,
            "unsupported intermediate range must fail explicitly");
    };
    const double minimum = std::numeric_limits<double>::min();
    const double maximum = std::numeric_limits<double>::max();
    {
        NetworkBuilder r;
        const auto a = r.add_node();
        r.add_resistor(a, ground, maximum); // Reciprocal is subnormal, not a silent open.
        range(r.finalize(), false);
    }
    {
        NetworkBuilder r;
        const auto a = r.add_node();
        for (int i = 0; i < 8; ++i) {
            r.add_resistor(a, ground, minimum); // Finite terms overflow during stamp accumulation.
        }
        range(r.finalize(), false);
    }
    range(b.finalize(), true, maximum); // omega overflow, even if no reactive element exists.
    for (bool inductor : {false, true}) {
        for (bool overflow : {false, true}) {
            NetworkBuilder reactive;
            const auto a = reactive.add_node();
            reactive.add_resistor(a, ground, 1);
            const double value = overflow ? maximum : minimum;
            if (inductor) {
                reactive.add_inductor(a, ground, value);
            } else {
                reactive.add_capacitor(a, ground, value);
            }
            range(reactive.finalize(), true, overflow ? 1 : minimum);
        }
    }
    {
        NetworkBuilder sources;
        const auto a = sources.add_node();
        sources.add_resistor(a, ground, 1);
        sources.add_current_source(ground, a, {maximum, {maximum, 0}});
        sources.add_current_source(ground, a, {maximum, {maximum, 0}});
        range(sources.finalize(), false);
        range(sources.finalize(), true);
    }
    for (double voltage : {1e160, 1e-200}) {
        NetworkBuilder power;
        const auto a = power.add_node();
        power.add_voltage_source(a, ground, {voltage, {voltage, 0}});
        power.add_resistor(a, ground, 1);
        range(power.finalize(), false); // v*i overflows or underflows, although inputs are finite.
        range(power.finalize(), true);
    }
    for (bool huge : {false, true}) {
        SolveOptions options;
        options.voltage_scale_v = huge ? maximum : minimum;
        options.current_scale_a = huge ? minimum : maximum;
        fails(
            [&] {
                (void)solve_dc(b.finalize(), options);
            },
            Failure::range_error,
            "unrepresentable dimensionless scaling is rejected");
    }
}

void ownership_repetition_orientation_and_scale() {
    static_assert(!std::is_default_constructible<Network>::value, "no unchecked network");
    static_assert(std::is_copy_constructible<Network>::value, "owning snapshots can be copied");
    static_assert(std::is_const<typename std::remove_reference<
                      decltype(std::declval<const Network &>().elements())>::type>::value,
                  "network component view is const");
    NetworkBuilder builder;
    const auto n = builder.add_node();
    SourceValue value{6, {3, 4}};
    const auto source = builder.add_voltage_source(n, ground, value);
    const auto resistor = builder.add_resistor(n, ground, 2);
    value = {100, {100, 100}};
    const auto first = builder.finalize();
    const auto copy = first;
    builder.add_resistor(n, ground, 6);
    const auto second = builder.finalize();
    const auto isolated = builder.add_node();
    require(isolated.value == 2, "node IDs remain stable after snapshots");
    require(first.node_count() == 2 && first.element_count() == 2,
            "snapshot owns original contents");
    require(second.node_count() == 2 && second.element_count() == 3,
            "later snapshot owns added element");
    near(
        solve_dc(first).elements[source.value].current, -3, "snapshot unaffected by builder edits");
    near(solve_dc(second).elements[source.value].current,
         -4,
         "new snapshot reflects additional load");
    near(solve_dc(copy).elements[resistor.value].current,
         3,
         "copied snapshot is independently valid");
    for (int repeat = 0; repeat < 4; ++repeat) {
        near(solve_ac(first, omega_one_hz).node_voltage[n.value], {3, 4}, "source drive is owned");
        near(solve_dc(first).node_voltage[n.value], 6, "repeat DC after AC does not mutate drive");
    }
    const auto escaped = [] {
        NetworkBuilder local;
        const auto a = local.add_node();
        local.add_current_source(ground, a, {2, {3, -1}});
        local.add_resistor(a, ground, 4);
        return local.finalize();
    }();
    near(solve_dc(escaped).node_voltage[1], 8, "network outlives builder storage");
    near(solve_ac(escaped, 1).node_voltage[1], {12, -4}, "AC network outlives builder storage");

    // Independent series formulas, with physical impedance and excitation scales varied separately.
    for (double impedance : {1e-9, 1e-3, 1.0, 1e3, 1e9}) {
        for (double excitation : {1e-9, 1.0, 1e9}) {
            for (bool reverse_order : {false, true}) {
                NetworkBuilder scaled;
                const auto first_node = scaled.add_node(), second_node = scaled.add_node();
                const auto a = reverse_order ? second_node : first_node;
                const auto z = reverse_order ? first_node : second_node;
                ElementId upper{0}, lower{0}, supply{0};
                if (reverse_order) {
                    lower = scaled.add_resistor(ground, z, impedance);
                    upper = scaled.add_resistor(z, a, 2 * impedance);
                    supply = scaled.add_voltage_source(
                        ground, a, {-12 * excitation, {-3 * excitation, -4 * excitation}});
                } else {
                    supply = scaled.add_voltage_source(
                        a, ground, {12 * excitation, {3 * excitation, 4 * excitation}});
                    upper = scaled.add_resistor(a, z, 2 * impedance);
                    lower = scaled.add_resistor(z, ground, impedance);
                }
                const auto network = scaled.finalize();
                const auto dc = solve_dc(network);
                const auto ac = solve_ac(network, omega_one_hz);
                const double orientation = reverse_order ? -1 : 1;
                near(dc.node_voltage[z.value],
                     4 * excitation,
                     "scale and node-order DC invariance",
                     1e-10,
                     1e-20);
                near(dc.elements[upper.value].current,
                     orientation * 4 * excitation / impedance,
                     "resistor reversal only changes oriented current",
                     1e-10,
                     1e-25);
                near(dc.elements[lower.value].voltage,
                     orientation * 4 * excitation,
                     "resistor reversal only changes terminal voltage",
                     1e-10,
                     1e-20);
                near(dc.elements[supply.value].current,
                     -orientation * 4 * excitation / impedance,
                     "source reversal and element reorder preserve physical solution",
                     1e-10,
                     1e-25);
                near(ac.node_voltage[z.value],
                     excitation * Complex{1, 4.0 / 3},
                     "scale and node-order AC invariance",
                     1e-10,
                     1e-20);
                near(ac.elements[upper.value].power,
                     50 * excitation * excitation / (9 * impedance),
                     "scale-sweep RMS resistor power",
                     1e-10,
                     1e-30);
                diagnostics_and_ids(dc, 3, 3);
                diagnostics_and_ids(ac, 3, 3);
                conserved_power(dc, "scaled reordered DC power balance");
                conserved_power(ac, "scaled reordered AC power balance");
            }
        }
    }
}

void maximum_size_acceptance_oracles() {
    // Sixteen complete eight-node resistor clusters. With one 1/R ground shunt per node,
    // hand KCL gives source I_k = -(9*V_k - sum(V_cluster))/R. Each node is also fixed
    // by its own source, giving exactly 128 node + 128 branch unknowns, with 704 elements.
    for (double impedance : {1e-9, 1.0, 1e9}) {
        NetworkBuilder b;
        std::vector<NodeId> nodes;
        std::vector<ElementId> sources;
        for (std::size_t i = 0; i < 128; ++i) {
            nodes.push_back(b.add_node());
        }
        for (std::size_t group = 0; group < 16; ++group) {
            const double amplitude = static_cast<double>(group + 1);
            for (std::size_t k = 0; k < 8; ++k) {
                const auto node = nodes[group * 8 + k];
                const double voltage = amplitude * static_cast<double>(k + 1);
                sources.push_back(
                    b.add_voltage_source(node, ground, {voltage, {voltage, 2 * voltage}}));
                b.add_resistor(node, ground, impedance);
                for (std::size_t j = k + 1; j < 8; ++j) {
                    b.add_resistor(node, nodes[group * 8 + j], impedance);
                }
            }
        }
        const auto network = b.finalize();
        const auto dc = solve_dc(network);
        const auto ac = solve_ac(network, omega_one_hz);
        diagnostics_and_ids(dc, 129, 704);
        diagnostics_and_ids(ac, 129, 704);
        for (std::size_t index = 0; index < 128; ++index) {
            const double amplitude = static_cast<double>(index / 8 + 1);
            const double local = static_cast<double>(index % 8 + 1);
            const double voltage = amplitude * local;
            const double current = amplitude * (36 - 9 * local) / impedance;
            near(dc.node_voltage[nodes[index].value], voltage, "256-state dense DC voltage oracle");
            near(ac.node_voltage[nodes[index].value],
                 {voltage, 2 * voltage},
                 "256-state dense complex voltage oracle");
            near(dc.elements[sources[index].value].current,
                 current,
                 "256-state dense DC branch current oracle",
                 1e-10,
                 1e-10 / impedance);
            near(ac.elements[sources[index].value].current,
                 {current, 2 * current},
                 "256-state dense complex branch current oracle",
                 1e-10,
                 1e-10 / impedance);
        }
        conserved_power(dc, "256-state dense DC power conservation");
        conserved_power(ac, "256-state dense AC power conservation");
    }
    // Sixty-four independent series RL cells, exactly 128 V + 64 iV + 64 iL unknowns.
    // This independently exercises both retained branch kinds at the maximum admitted size.
    for (double impedance : {1e-6, 1.0, 1e6}) {
        NetworkBuilder b;
        std::vector<NodeId> load_nodes;
        std::vector<ElementId> sources, inductors;
        for (std::size_t k = 0; k < 64; ++k) {
            const auto supply = b.add_node(), load = b.add_node();
            const double excitation = static_cast<double>(k + 1);
            load_nodes.push_back(load);
            sources.push_back(
                b.add_voltage_source(supply, ground, {10 * excitation, {10 * excitation, 0}}));
            b.add_resistor(supply, load, 2 * impedance);
            inductors.push_back(b.add_inductor(load, ground, 3 * impedance));
        }
        const auto network = b.finalize();
        const auto dc = solve_dc(network);
        const auto ac = solve_ac(network, omega_one_hz);
        diagnostics_and_ids(dc, 129, 192);
        diagnostics_and_ids(ac, 129, 192);
        for (std::size_t k = 0; k < 64; ++k) {
            const double excitation = static_cast<double>(k + 1);
            near(dc.node_voltage[load_nodes[k].value], 0, "256-state DC L exact-short oracle");
            near(dc.elements[inductors[k].value].current,
                 5 * excitation / impedance,
                 "256-state DC retained L current oracle");
            const Complex current = excitation / impedance * Complex{20.0 / 13, -30.0 / 13};
            near(ac.node_voltage[load_nodes[k].value],
                 excitation * Complex{90.0 / 13, 60.0 / 13},
                 "256-state AC RL voltage oracle");
            near(ac.elements[inductors[k].value].current, current, "256-state AC L current oracle");
            near(ac.elements[sources[k].value].current, -current, "256-state AC V current oracle");
        }
        conserved_power(dc, "256-state RL DC power conservation");
        conserved_power(ac, "256-state RL AC power conservation");
    }
}

void empty_zero_rhs_and_budgets() {
    NetworkBuilder empty;
    const auto ground_only = empty.finalize();
    const auto dc = solve_dc(ground_only);
    const auto ac = solve_ac(ground_only, 1);
    diagnostics_and_ids(dc, 1, 0);
    diagnostics_and_ids(ac, 1, 0);
    require(dc.diagnostics.max_scaled_residual == 0, "empty solve zero residual");
    SolveOptions invalid;
    invalid.voltage_scale_v = 0;
    fails(
        [&] {
            (void)solve_dc(ground_only, invalid);
        },
        Failure::invalid_input,
        "empty solve still validates policy");
    fails(
        [&] {
            (void)solve_ac(ground_only, 0);
        },
        Failure::invalid_input,
        "empty solve still validates AC frequency");

    NetworkBuilder zero;
    const auto a = zero.add_node(), b = zero.add_node();
    zero.add_voltage_source(a, ground, {});
    zero.add_resistor(a, b, 2);
    zero.add_inductor(b, ground, 3);
    zero.add_capacitor(b, ground, .5);
    zero.add_current_source(b, ground, {});
    for (const auto &e : solve_dc(zero.finalize()).elements) {
        require(e.voltage == 0 && e.current == 0 && e.power == 0, "exact zero DC RHS result");
    }
    const auto zero_ac = solve_ac(zero.finalize(), omega_one_hz);
    for (const auto &e : zero_ac.elements) {
        require(e.voltage == Complex{} && e.current == Complex{} && e.power == Complex{},
                "exact zero AC RHS result");
    }
    require(zero_ac.diagnostics.max_scaled_residual == 0,
            "zero RHS denominator case residual zero");

    NetworkBuilder max_nodes;
    for (std::size_t i = 1; i <= 256; ++i) {
        const auto node = max_nodes.add_node();
        require(node.value == i, "bounded sequential node allocation");
        max_nodes.add_resistor(node, ground, 1);
    }
    const auto full = max_nodes.finalize();
    diagnostics_and_ids(solve_dc(full), 257, 256);
    fails(
        [&] {
            (void)max_nodes.add_node();
        },
        Failure::budget_exceeded,
        "257th non-ground node exceeds budget");
    max_nodes.add_voltage_source(NodeId{1}, ground, {});
    fails(
        [&] {
            (void)max_nodes.finalize();
        },
        Failure::budget_exceeded,
        "branch currents count toward total unknown budget");
    require(full.element_count() == 256, "budget failure cannot mutate existing snapshot");

    NetworkBuilder boundary;
    const auto driven = boundary.add_node();
    boundary.add_voltage_source(driven, ground, {2, {2, 0}});
    boundary.add_resistor(driven, ground, 1);
    for (std::size_t i = 1; i < 255; ++i) {
        const auto node = boundary.add_node();
        boundary.add_resistor(node, ground, 1);
    }
    const auto exactly_256 = solve_dc(boundary.finalize());
    near(exactly_256.node_voltage[driven.value], 2, "255 voltages plus branch exactly at limit");
    boundary.add_inductor(driven, ground, 1);
    fails(
        [&] {
            (void)boundary.finalize();
        },
        Failure::budget_exceeded,
        "inductor branch current counts against unknown budget");

    NetworkBuilder many;
    const auto n = many.add_node();
    for (std::size_t i = 0; i < 4096; ++i) {
        require(many.add_resistor(n, ground, 4096).value == i, "element ID at budget boundary");
    }
    diagnostics_and_ids(solve_dc(many.finalize()), 2, 4096);
    fails(
        [&] {
            (void)many.add_current_source(n, ground, {});
        },
        Failure::budget_exceeded,
        "4097th element exceeds budget");
    require(many.finalize().element_count() == 4096, "element budget failure has strong guarantee");
}
} // namespace

int main() {
    try {
        divider_and_floating_source();
        reactive_oracles_and_dc_limits();
        current_polarity_and_ac_without_dc();
        singular_topologies();
        options_and_near_singular();
        input_domain_and_arithmetic_range();
        ownership_repetition_orientation_and_scale();
        maximum_size_acceptance_oracles();
        empty_zero_rhs_and_budgets();
        std::cout << "linear MNA contract: " << checks << " checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << "linear MNA contract failed after " << checks << " checks: " << error.what()
                  << '\n';
        return 1;
    }
    return 0;
}
