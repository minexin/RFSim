#include "rfmodel/nonlinear/dc_operating_point.hpp"
#include <iostream>
#include <iomanip>
#include <memory>
#include <type_traits>

namespace {
using namespace rfmodel;
std::size_t checks = 0;

void require(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(double actual, double expected, double tolerance, const char *message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

template <class F> void failure(F action, DcSolveFailure expected) {
    ++checks;
    try {
        action();
    } catch (const DcSolveError &e) {
        if (e.failure() == expected) {
            return;
        }
        throw;
    }
    throw std::runtime_error("missing structured DC failure");
}

template <class F> void invalid(F action) {
    ++checks;
    try {
        action();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("missing invalid argument");
}

DcDevice branch(DcNode p, DcNode m, double g) {
    return {"resistor", {p, m}, [g](const std::vector<double> &v) {
                const double i = g * (v[0] - v[1]);
                return DcDeviceEvaluation{{i, -i}, {g, -g, -g, g}, std::nullopt};
            }};
}

void tests() {
    static_assert(!std::is_default_constructible<DcOperatingPoint>::value, "no unchecked point");
    DcNetwork divider{1, {branch(1, 0, .001), branch(1, 0, .0005)}, {.003}};
    const auto linear = solve_dc(divider, {0});
    near(linear.voltage_volt()[0], 2, 1e-14, "divider voltage");
    near(linear.device_evaluations()[1].current_ampere[0], .001, 1e-17, "load current sign");
    near(linear.conductance_siemens()[0], .0015, 1e-18, "source loading remains");
    require(linear.iterations() == 1 && linear.evaluations() == 2, "linear batch counts");
    DcSolveOptions one;
    one.max_newton_iterations = 1;
    near(
        solve_dc(divider, {0}, one).voltage_volt()[0], 2, 1e-14, "last update convergence checked");

    std::size_t calls = 0;
    DcNetwork cubic{1,
                    {branch(1, 0, .001),
                     {"cubic",
                      {1, 0},
                      [&](const std::vector<double> &v) {
                          ++calls;
                          require(v[1] == 0 && !std::signbit(v[1]), "exact positive ground zero");
                          const double x = v[0], i = .001 * x + .002 * x * x * x,
                                       g = .001 + .006 * x * x;
                          const double q = 1e-9 * x + 3e-9 * x * x * x, c = 1e-9 + 9e-9 * x * x;
                          return DcDeviceEvaluation{
                              {i, -i}, {g, -g, -g, g}, DcChargeEvaluation{{q, -q}, {c, -c, -c, c}}};
                      }}},
                    {.002}};
    double lo = 0, hi = 2;
    for (int i = 0; i < 100; ++i) {
        const double mid = (lo + hi) / 2;
        if (mid * mid * mid + mid > 1) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const double root = (lo + hi) / 2;
    const auto point = solve_dc(cubic, {0});
    near(point.voltage_volt()[0], root, 1e-10, "independent bisection root");
    near(point.conductance_siemens()[0], .002 + .006 * root * root, 1e-12, "bias G");
    near(point.capacitance_farad()[0], 1e-9 + 9e-9 * root * root, 1e-18, "bias C");
    near(point.device_evaluations()[1].charge->charge_coulomb[0],
         1e-9 * root + 3e-9 * root * root * root,
         1e-18,
         "accepted Q");
    require(calls == point.evaluations(), "exact retained final batch, no reevaluation");
    require(point.current_convergence_norm() <= 1 && point.voltage_convergence_norm() <= 1,
            "unit-aware norms");
    DcSolveOptions backtrack_options;
    backtrack_options.armijo = .8;
    const auto backtrack = solve_dc(cubic, {0}, backtrack_options);
    require(backtrack.evaluations() > backtrack.iterations() + 1, "Armijo backtrack used");
    near(backtrack.voltage_volt()[0], root, 1e-8, "backtrack recovers");
    const auto large = solve_dc(cubic, {50});
    near(large.voltage_volt()[0], root, 1e-9, "large starting voltage");
    failure(
        [&] {
            solve_dc(cubic, {0}, one);
        },
        DcSolveFailure::iteration_limit);

    DcNetwork two{2, {branch(1, 0, .001), branch(1, 2, .002), branch(2, 0, .003)}, {.003, 0}};
    auto two_point = solve_dc(two, {0, 0});
    near(two_point.voltage_volt()[0], 15. / 11, 1e-14, "coupled V1");
    near(two_point.voltage_volt()[1], 6. / 11, 1e-14, "coupled V2");
    DcNetwork tied{1, {branch(1, 0, .001), branch(1, 1, 20)}, {.001}};
    near(solve_dc(tied, {0}).voltage_volt()[0], 1, 2e-12, "tied terminal accumulation");
    DcNetwork tiny{1, {branch(1, 0, 1e-15)}, {2e-15}};
    DcSolveOptions tiny_options;
    tiny_options.current_scale_ampere = {1e-15};
    tiny_options.voltage_scale_volt = {2};
    tiny_options.current_absolute_tolerance_ampere = 1e-27;
    near(solve_dc(tiny, {0}, tiny_options).voltage_volt()[0], 2, 1e-14, "no one siemens floor");
    tiny_options.current_scale_ampere = {1e-6};
    tiny_options.voltage_scale_volt = {1e3};
    near(solve_dc(tiny, {0}, tiny_options).voltage_volt()[0], 2, 1e-14, "alternate scales");

    // On MSVC long double has double range. Never let a scaled nonzero RHS
    // underflow to zero and falsely satisfy both convergence criteria.
    DcNetwork extreme{1, {branch(1, 0, 1e-300)}, {1e-200}};
    DcSolveOptions extreme_options;
    extreme_options.current_scale_ampere = {1e200};
    extreme_options.voltage_scale_volt = {1e200};
    extreme_options.voltage_relative_tolerance = 0;
    failure(
        [&] {
            solve_dc(extreme, {0}, extreme_options);
        },
        DcSolveFailure::numeric_range);

    // A rounded subnormal scaled RHS must not shrink a 7 V correction below
    // a 6 V acceptance threshold (even if the chosen current tolerance is huge).
    extreme = DcNetwork{1, {branch(1, 0, 1e-200)}, {7e-200}};
    extreme_options.current_scale_ampere = {1e124};
    extreme_options.voltage_scale_volt = {1e124};
    extreme_options.voltage_absolute_tolerance_volt = 6;
    failure(
        [&] {
            solve_dc(extreme, {0}, extreme_options);
        },
        DcSolveFailure::numeric_range);

    DcNetwork floating{2, {branch(1, 2, .001)}, {0, 0}};
    failure(
        [&] {
            solve_dc(floating, {0, 0});
        },
        DcSolveFailure::singular_jacobian);
    DcNetwork disconnected{2, {branch(1, 0, .001)}, {0, 0}};
    failure(
        [&] {
            solve_dc(disconnected, {0, 0});
        },
        DcSolveFailure::singular_jacobian);
    DcNetwork cancellation{1, {branch(1, 0, 1), branch(1, 0, -1)}, {0}};
    failure(
        [&] {
            solve_dc(cancellation, {0});
        },
        DcSolveFailure::singular_jacobian);
    DcNetwork pure_cubic{1,
                         {{"pure cubic",
                           {1},
                           [](const std::vector<double> &v) {
                               return DcDeviceEvaluation{
                                   {v[0] * v[0] * v[0]}, {3 * v[0] * v[0]}, std::nullopt};
                           }}},
                         {0}};
    failure(
        [&] {
            solve_dc(pure_cubic, {0});
        },
        DcSolveFailure::singular_jacobian);
    DcNetwork no_root{1,
                      {{"exp plus one",
                        {1},
                        [](const std::vector<double> &v) {
                            const double x = std::exp(v[0]);
                            return DcDeviceEvaluation{{x + 1}, {x}, std::nullopt};
                        }}},
                      {0}};
    ++checks;
    try {
        solve_dc(no_root, {0});
        throw std::logic_error("no root accepted");
    } catch (const DcSolveError &) {
    }
    DcNetwork inconsistent{1,
                           {{"intentionally false derivative",
                             {1},
                             [](const std::vector<double> &) {
                                 return DcDeviceEvaluation{{1}, {1}, std::nullopt};
                             }}},
                           {0}};
    failure(
        [&] {
            solve_dc(inconsistent, {0});
        },
        DcSolveFailure::line_search_failed);
    DcSolveOptions minute_step;
    minute_step.backtrack_factor = 1e-20;
    minute_step.max_backtracks = 1;
    failure(
        [&] {
            solve_dc(inconsistent, {0}, minute_step);
        },
        DcSolveFailure::line_search_failed);
    int domain_calls = 0, model_calls = 0;
    DcNetwork logarithm{1,
                        {{"log",
                          {1},
                          [&](const std::vector<double> &v) {
                              ++model_calls;
                              require(v[0] > 0, "invalid domain never evaluated");
                              return DcDeviceEvaluation{{std::log(v[0])}, {1 / v[0]}, std::nullopt};
                          },
                          [&](const std::vector<double> &v) {
                              ++domain_calls;
                              return v[0] > 0;
                          }}},
                        {0}};
    const auto domain_point = solve_dc(logarithm, {10});
    near(domain_point.voltage_volt()[0], 1, 1e-9, "explicit domain backtrack recovery");
    require(domain_calls > model_calls &&
                static_cast<std::size_t>(model_calls) == domain_point.evaluations(),
            "domain rejects excluded from batches");
    failure(
        [&] {
            solve_dc(logarithm, {0});
        },
        DcSolveFailure::domain_violation);
    logarithm.devices[0].in_domain = [](const std::vector<double> &) -> bool {
        throw 5;
    };
    failure(
        [&] {
            solve_dc(logarithm, {1});
        },
        DcSolveFailure::callback_failed);
    for (bool nonstandard : {false, true}) {
        int throws = 0;
        auto throwing = divider;
        throwing.devices[0].evaluate = [&](const std::vector<double> &) -> DcDeviceEvaluation {
            ++throws;
            if (nonstandard) {
                throw 7;
            }
            throw std::runtime_error("model failed");
        };
        ++checks;
        try {
            solve_dc(throwing, {0});
            throw std::logic_error("throw ignored");
        } catch (const DcSolveError &e) {
            require(e.failure() == DcSolveFailure::callback_failed &&
                        std::string(e.what()).find("resistor") != std::string::npos,
                    "callback context");
            bool nested = false;
            try {
                std::rethrow_if_nested(e);
            } catch (...) {
                nested = true;
            }
            require(nested && throws == 1, "nested callback and no retry");
        }
    }
    for (int mode = 0; mode < 8; ++mode) {
        auto malformed = divider;
        malformed.devices[0].evaluate = [mode](const std::vector<double> &) {
            DcDeviceEvaluation e{{0, 0}, {1, -1, -1, 1}, std::nullopt};
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            if (mode == 0) {
                e.current_ampere.pop_back();
            }
            if (mode == 1) {
                e.dcurrent_dvoltage_siemens.pop_back();
            }
            if (mode == 2) {
                e.current_ampere[1] = nan;
            }
            if (mode == 3) {
                e.dcurrent_dvoltage_siemens[3] = std::numeric_limits<double>::infinity();
            }
            if (mode >= 4) {
                e.charge = DcChargeEvaluation{{0, 0}, {1, -1, -1, 1}};
            }
            if (mode == 4) {
                e.charge->charge_coulomb.pop_back();
            }
            if (mode == 5) {
                e.charge->dcharge_dvoltage_farad.pop_back();
            }
            if (mode == 6) {
                e.charge->charge_coulomb[1] = nan;
            }
            if (mode == 7) {
                e.charge->dcharge_dvoltage_farad[3] = -std::numeric_limits<double>::infinity();
            }
            return e;
        };
        failure(
            [&] {
                solve_dc(malformed, {0});
            },
            DcSolveFailure::callback_failed);
    }
    DcNetwork overflow{1, {branch(1, 0, 1e308), branch(1, 0, 1e308)}, {0}};
    failure(
        [&] {
            solve_dc(overflow, {0});
        },
        DcSolveFailure::numeric_range);
    for (int mode = 0; mode < 11; ++mode) {
        auto bad = divider;
        if (mode == 0) {
            bad.non_ground_nodes = 0;
        }
        if (mode == 1) {
            bad.non_ground_nodes = 1025;
        }
        if (mode == 2) {
            bad.injection_ampere.clear();
        }
        if (mode == 3) {
            bad.injection_ampere[0] = std::numeric_limits<double>::infinity();
        }
        if (mode == 4) {
            bad.devices[0].terminals.clear();
        }
        if (mode == 5) {
            bad.devices[0].terminals = {2};
        }
        if (mode == 6) {
            bad.devices[0].evaluate = {};
        }
        if (mode == 7) {
            bad.devices[0].terminals.resize(1025);
        }
        invalid([&] {
            solve_dc(bad,
                     mode == 8    ? std::vector<double>{}
                     : mode == 9  ? std::vector<double>{0, 0}
                     : mode == 10 ? std::vector<double>{std::numeric_limits<double>::quiet_NaN()}
                                  : std::vector<double>{0});
        });
    }
    for (int mode = 0; mode < 14; ++mode) {
        DcSolveOptions o;
        if (mode == 0) {
            o.current_scale_ampere = {0};
        }
        if (mode == 1) {
            o.voltage_scale_volt = {-1};
        }
        if (mode == 2) {
            o.current_scale_ampere = {1, 2};
        }
        if (mode == 3) {
            o.voltage_scale_volt = {std::numeric_limits<double>::infinity()};
        }
        if (mode == 4) {
            o.current_absolute_tolerance_ampere = 0;
        }
        if (mode == 5) {
            o.voltage_absolute_tolerance_volt = -1;
        }
        if (mode == 6) {
            o.current_relative_tolerance = -1;
        }
        if (mode == 7) {
            o.voltage_relative_tolerance = std::numeric_limits<double>::quiet_NaN();
        }
        if (mode == 8) {
            o.armijo = 1;
        }
        if (mode == 9) {
            o.backtrack_factor = 0;
        }
        if (mode == 10) {
            o.max_newton_iterations = 0;
        }
        if (mode == 11) {
            o.max_backtracks = 10001;
        }
        if (mode == 12) {
            o.current_relative_tolerance = 1e308;
            o.current_scale_ampere = {1e308};
        }
        if (mode == 13) {
            o.voltage_relative_tolerance = 1e308;
            o.voltage_scale_volt = {1e308};
        }
        invalid([&] {
            solve_dc(divider, {0}, o);
        });
    }
    near(linear.voltage_volt()[0], 2, 1e-14, "prior result survives all failures");
    std::cout << std::setprecision(17) << "root=" << point.voltage_volt()[0]
              << " G=" << point.conductance_siemens()[0] << " C=" << point.capacitance_farad()[0]
              << " updates=" << point.iterations() << " batches=" << point.evaluations() << '\n';
}
} // namespace

int main() {
    try {
        tests();
        std::cout << checks << " DC checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
