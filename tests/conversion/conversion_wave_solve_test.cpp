#include "rfmodel/conversion/conversion_network.hpp"
#include "rfmodel/conversion/conversion_power_wave.hpp"
#include "../support/test_support.hpp"
#include <iostream>
#include <limits>

using namespace rfmodel;
using Vector = std::vector<Complex>;
using Connections = std::vector<std::pair<std::size_t, std::size_t>>;

static void compare(const ConversionWaveResult &waves, const ConversionResult &full) {
    require(waves.incident.size() == full.incident.size() &&
                waves.outgoing.size() == full.outgoing.size(),
            "wave-only dimensions differ");
    require(std::isfinite(waves.relative_residual) && waves.relative_residual >= 0. &&
                waves.relative_residual < 1e-12,
            "wave-only residual invalid");
    for (std::size_t i = 0; i < waves.incident.size(); ++i) {
        near(waves.incident[i], full.incident[i]);
        near(waves.outgoing[i], full.outgoing[i]);
    }
}

static void scalar_feedback() {
    const Complex source{.4, -.2}, offset{-.3, .5}, gamma{.2, .1};
    // A-only, B-only and widely-linear feedback have an independent scalar oracle.
    for (const auto &coefficients : {std::pair<Complex, Complex>{{.3, .1}, {}},
                                     std::pair<Complex, Complex>{{}, {.12, -.04}},
                                     std::pair<Complex, Complex>{{.3, .1}, {.12, -.04}}}) {
        const auto a = coefficients.first, b = coefficients.second;
        FrequencyConversionModel model(1., {{0, 1}}, {1, {a}}, {1, {b}});
        const auto zero = model.zero_noise();
        for (const Vector &offsets : {Vector{}, Vector{offset}}) {
            const auto result = model.solve_waves({source}, {gamma}, {}, offsets);
            const auto u = 1. - a * gamma, q = b * std::conj(gamma);
            const auto z =
                a * source + b * std::conj(source) + (offsets.empty() ? Complex{} : offset);
            const auto expected =
                (std::conj(u) * z + q * std::conj(z)) / (std::norm(u) - std::norm(q));
            near(result.outgoing[0], expected);
            near(result.incident[0], source + gamma * expected);
            compare(result, model.analyze({source}, {gamma}, zero, zero, {}, true, offsets));
        }
        compare(model.solve_waves({0.}, {gamma}), model.analyze({0.}, {gamma}, zero, zero));
    }
}

static void dc_and_mixed_references() {
    // DC's missing imaginary degree of freedom coexists with conjugate RF coupling.
    const FrequencyConversionModel model(
        1.,
        {{0, 0}, {1, 1}},
        {2, {.2, Complex{.1, .03}, Complex{.04, -.02}, Complex{.3, .1}}},
        {2, {.1, Complex{.1, -.03}, Complex{.02, .01}, Complex{.08, -.02}}});
    const Vector source{.4, Complex{.2, -.3}}, reflection{.1, Complex{.2, .05}},
        offset{.03, Complex{-.1, .04}};
    const auto zero = model.zero_noise();
    const auto waves = model.solve_waves(source, reflection, {}, offset);
    compare(waves, model.analyze(source, reflection, zero, zero, {}, true, offset));
    near(waves.outgoing[0].imag(), 0.);
    near(waves.incident[0].imag(), 0.);
    for (const Vector &bad : {Vector{Complex{0., .1}, 0.}, Vector{0.}, Vector{0., INFINITY}}) {
        rejects<std::invalid_argument>([&] {
            model.solve_waves(bad, reflection);
        });
        rejects<std::invalid_argument>([&] {
            model.solve_waves(source, bad);
        });
        rejects<std::invalid_argument>([&] {
            model.solve_waves(source, reflection, {}, bad);
        });
    }

    // The same physical reflection loop in unequal complex local references.
    const FrequencyConversionModel first(1., {{0, 1}}, {1, {Complex{.2, .1}}}, {1, {.05}});
    const FrequencyConversionModel second(1., {{0, 1}}, {1, {Complex{-.1, .04}}}, {1, {.03}});
    const auto one = renormalize_power_waves(first, {{30., 11.}});
    const auto two = renormalize_power_waves(second, {{80., -17.}});
    FrequencyConversionNetwork original(1.), changed(1.);
    original.add(first);
    original.add(second);
    original.connect(0, 0, 1, 0);
    changed.add_power_waves(one.model);
    changed.add_power_waves(two.model);
    changed.connect(0, 0, 1, 0);
    const Vector original_offset{{.3, -.1}, {-.2, .4}};
    const auto x = renormalize_conversion_offset(one, {original_offset[0]});
    const auto y = renormalize_conversion_offset(two, {original_offset[1]});
    const Vector changed_offset{x[0], y[0]};
    const auto result = changed.solve_waves(changed_offset);
    compare(result, changed.analyze(true, nullptr, changed_offset));
    const auto reference = original.solve_waves(original_offset);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto z = changed.reference_impedances_ohms()[i];
        const auto v =
            (std::conj(z) * result.incident[i] + z * result.outgoing[i]) / std::sqrt(z.real());
        const auto current = (result.incident[i] - result.outgoing[i]) / std::sqrt(z.real());
        near(v, std::sqrt(50.) * (reference.incident[i] + reference.outgoing[i]));
        near(current, (reference.incident[i] - reference.outgoing[i]) / std::sqrt(50.));
    }
}

static void analytic_wire_loop() {
    const Complex a{.3, .1}, b{-.2, .05}, e{.4, -.3};
    FrequencyConversionNetwork network(1.);
    network.add(FrequencyConversionModel(1., {{0, 1}}, {1, {a}}, {1, {0.}}));
    network.add(FrequencyConversionModel(1., {{0, 1}}, {1, {b}}, {1, {0.}}));
    network.connect(0, 0, 1, 0);
    const auto result = network.solve_waves({e, 0.});
    const auto first = e / (1. - a * b), second = b * first;
    near(result.outgoing[0], first);
    near(result.outgoing[1], second);
    near(result.incident[0], second);
    near(result.incident[1], first);
    compare(result, network.analyze(true, nullptr, {e, 0.}));
    const FrequencyConversionModel model(1., {{0, 1}, {1, 1}}, {2, {a, 0., 0., b}}, {2, Vector(4)});
    compare(
        model.solve_waves(Vector(2), Vector(2), {{0, 1}}, {e, 0.}),
        model.analyze(
            Vector(2), Vector(2), model.zero_noise(), model.zero_noise(), {{0, 1}}, true, {e, 0.}));
}

// No absolute tolerance: losing a 1e-317 signal must fail this regression.
static void extreme_relative(double actual, double expected) {
    require(std::isfinite(actual) && std::isfinite(expected), "nonfinite extreme wave");
    if (expected == 0.) {
        require(actual == expected, "zero extreme wave changed");
    } else {
        require(std::abs((actual - expected) / expected) < 2e-12,
                "extreme deterministic wave lost relative precision");
    }
}

static void extreme_dynamic_range() {
    const auto check = [](const FrequencyConversionModel &model,
                          const Vector &source,
                          const Vector &reflection,
                          const Vector &offset = Vector{}) {
        const auto zero = model.zero_noise();
        // This must succeed first; these cases protect existing successful results.
        const auto full = model.analyze(source, reflection, zero, zero, {}, false, offset);
        const auto fast = model.solve_waves(source, reflection, {}, offset);
        require(std::isfinite(fast.relative_residual) && fast.relative_residual < 1e-12,
                "extreme wave residual invalid");
        for (std::size_t i = 0; i < source.size(); ++i) {
            extreme_relative(fast.incident[i].real(), full.incident[i].real());
            extreme_relative(fast.incident[i].imag(), full.incident[i].imag());
            extreme_relative(fast.outgoing[i].real(), full.outgoing[i].real());
            extreme_relative(fast.outgoing[i].imag(), full.outgoing[i].imag());
        }
        return full;
    };
    const double tiny = std::numeric_limits<double>::denorm_min();
    for (int bin : {0, 1}) {
        const FrequencyConversionModel scalar(1., {{0, bin}}, {1, {1e-200}}, {1, {0.}});
        for (double gap : {1e-12, 1e-13}) {
            const Vector reflection{(1. - gap) / 1e-200};
            for (double source : {1e-130, 7e-124}) {
                // The first Q*source rounds to zero; the second rounds to a nonzero subnormal.
                const auto full = check(scalar, {source}, reflection);
                require(full.outgoing[0].real() > 0., "underflow fixture lost its signal");
            }
            const auto cancelled = check(scalar, {7e-124}, reflection, {-tiny});
            require(cancelled.outgoing[0].real() > 0., "offset cancellation fixture became zero");
        }
        const FrequencyConversionModel unrelated(
            1., {{0, bin}, {1, 1}}, {2, {1e-200, 0., 0., 0.}}, {2, Vector(4)});
        for (double source : {1e-130, 7e-124}) {
            // An unrelated large source must not hide precision loss in the weak channel.
            check(unrelated, {source, 1.}, {(1. - 1e-13) / 1e-200, 0.});
        }
        // RHS construction is safe here; the loss happens later during elimination.
        const FrequencyConversionModel elimination(
            1., {{0, bin}, {1, bin}}, {2, {0., 0., -1e-200, 1. - 1e-12}}, {2, Vector(4)});
        const auto eliminated = check(elimination, {0., 0.}, {1., 1.}, {1e-130, 0.});
        require(eliminated.outgoing[1].real() != 0., "elimination fixture lost its signal");
        const FrequencyConversionModel cancelling(
            1., {{0, bin}, {1, bin}}, {2, {1e200, -1e200, 0., 1e200}}, {2, Vector(4)});
        check(cancelling, {2e108, 2e108}, {-1e-199, -1e-199});
        const FrequencyConversionModel offset_model(1., {{0, bin}}, {1, {1e200}}, {1, {0.}});
        check(offset_model, {2e108}, {-1e-199}, {-1e307});
    }
}

static void validation() {
    const FrequencyConversionModel model(
        1., {{0, 1}, {1, 1}, {2, 2}}, {3, Vector(9)}, {3, Vector(9)});
    const Vector zeros(3);
    for (const Connections &bad : {Connections{{0, 0}},
                                   Connections{{0, 3}},
                                   Connections{{0, 1}, {1, 0}},
                                   Connections{{0, 2}}}) {
        rejects<std::invalid_argument>([&] {
            model.solve_waves(zeros, zeros, bad);
        });
    }
    rejects<std::invalid_argument>([&] {
        model.solve_waves({1., 0., 0.}, zeros, {{0, 1}});
    });
    rejects<std::invalid_argument>([&] {
        model.solve_waves(zeros, {1., 0., 0.}, {{0, 1}});
    });
    const FrequencyConversionModel singular(1., {{0, 1}}, {1, {1.}}, {1, {0.}});
    for (Complex source : {Complex{}, Complex{1.}}) {
        rejects<std::domain_error>([&] {
            singular.solve_waves({source}, {1.});
        });
    }
    rejects<std::invalid_argument>([] {
        FrequencyConversionNetwork(1.).solve_waves();
    });
    // Wave-only execution deliberately has no C/P contract. Full analysis still rejects invalid noise.
    auto bad_noise = model.zero_noise();
    bad_noise.covariance(0, 0) = -1.;
    FrequencyConversionNetwork network(1.);
    network.add({model, zeros, zeros, bad_noise, model.zero_noise()});
    const auto waves = network.solve_waves();
    for (auto value : waves.outgoing) {
        near(value, 0.);
    }
    rejects<std::invalid_argument>([&] {
        network.analyze();
    });
}

int main() {
    try {
        scalar_feedback();
        dc_and_mixed_references();
        analytic_wire_loop();
        extreme_dynamic_range();
        validation();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
