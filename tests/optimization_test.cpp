#include "rfmodel/optimization.hpp"
#include "rfmodel/network.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"
#include <cstring>

using namespace rfmodel;

int main() {
    OptimizationOptions options;
    options.minimum_step = 1e-9;
    options.target = 1e-15;
    const std::vector<OptimizationParameter> mixed{
        {-2., 2., .2},
        {1e-9, 1e3, 1e-6, OptimizationParameterKind::LogReal},
        {0., 10., 1., OptimizationParameterKind::Integer},
        {4., 4., 4.}};
    const auto objective = [](const std::vector<double> &x) {
        return (x[0] - .3) * (x[0] - .3) + std::pow(std::log10(x[1]) + 3., 2) +
               (x[2] - 7.) * (x[2] - 7.);
    };
    const auto fit = minimize_bounded(mixed, objective, options);
    require(fit.stop == OptimizationStop::TargetReached && fit.has_finite_point, "mixed target");
    require(std::abs(fit.parameters[0] - .3) < 1e-7 &&
                std::abs(fit.parameters[1] / 1e-3 - 1) < 1e-7,
            "mixed optimum");
    require(fit.parameters[2] == 7. && fit.parameters[3] == 4., "integer and fixed point");
    require(fit.evaluations == fit.history.size() && fit.rejected_evaluations == 0, "trace counts");
    for (const auto &e : fit.history) {
        require(e.status == OptimizationEvaluationStatus::Finite, "finite trace");
        for (std::size_t i = 0; i < mixed.size(); ++i) {
            require(e.parameters[i] >= mixed[i].lower && e.parameters[i] <= mixed[i].upper,
                    "feasible objective input");
        }
        require(std::trunc(e.parameters[2]) == e.parameters[2], "integer callback input");
        near(e.value, objective(e.parameters));
    }
    const auto replay = minimize_bounded(mixed, objective, options);
    require(replay.parameters == fit.parameters && replay.value == fit.value &&
                replay.stop == fit.stop && replay.evaluations == fit.evaluations &&
                replay.iterations == fit.iterations,
            "replay summary");
    for (std::size_t i = 0; i < fit.history.size(); ++i) {
        require(fit.history[i].parameters == replay.history[i].parameters &&
                    fit.history[i].value == replay.history[i].value,
                "replay evaluation trace");
    }
    options.record_history = false;
    const auto compact = minimize_bounded(mixed, objective, options);
    require(compact.history.empty() && compact.parameters == fit.parameters &&
                compact.evaluations == fit.evaluations,
            "trace option does not change search");

    // A constrained optimum on each bound; no differentiability is required.
    options = {};
    options.minimum_step = 1e-9;
    auto bound = minimize_bounded(
        {{0., 1., .5}, {-1., 2., 0.}},
        [](const auto &x) {
            return -x[0] + std::abs(x[1] + 1.);
        },
        options);
    require(bound.stop == OptimizationStop::StepToleranceReached, "bound stop");
    near(bound.parameters[0], 1.);
    near(bound.parameters[1], -1.);
    near(bound.value, -1.);
    auto fixed = minimize_bounded({{3., 3., 3.}}, [](const auto &x) {
        return x[0];
    });
    require(fixed.stop == OptimizationStop::AllParametersFixed && fixed.evaluations == 1 &&
                fixed.iterations == 0,
            "fixed request");

    // Complete polling selects the best of both directions, not the first improvement.
    options = {};
    options.max_iterations = 1;
    auto full_poll = minimize_bounded(
        {{0., 1., .5}},
        [](const auto &x) {
            return -std::abs(x[0] - .5) + .1 * (x[0] - .5);
        },
        options);
    require(full_poll.evaluations == 3 && full_poll.parameters[0] == .25,
            "complete poll chooses the better direction");

    // Budget stops retain the best evaluated point; incomplete polls are counted.
    options = {};
    options.max_evaluations = 2;
    auto limited = minimize_bounded(
        {{0., 1., 0.}},
        [](const auto &x) {
            return -x[0];
        },
        options);
    require(limited.stop == OptimizationStop::EvaluationLimit && limited.evaluations == 2 &&
                limited.iterations == 2,
            "evaluation cap");
    near(limited.parameters[0], .25);
    options = {};
    options.max_iterations = 1;
    limited = minimize_bounded(
        {{0., 1., 0.}},
        [](const auto &x) {
            return -x[0];
        },
        options);
    require(limited.stop == OptimizationStop::IterationLimit && limited.evaluations == 2 &&
                limited.iterations == 1,
            "iteration cap");
    options.target = -.25;
    limited = minimize_bounded(
        {{0., 1., 0.}},
        [](const auto &x) {
            return -x[0];
        },
        options);
    require(limited.stop == OptimizationStop::TargetReached,
            "target takes precedence after final evaluation");

    auto failure = minimize_bounded({{0., 1., .5}}, [](const auto &x) -> double {
        if (x[0] > .5) {
            throw std::domain_error("invalid circuit operating point");
        }
        return x[0];
    });
    require(failure.stop == OptimizationStop::ObjectiveFailure && failure.evaluations == 2 &&
                failure.has_finite_point,
            "failure preserves prior point");
    near(failure.parameters[0], .5);
    require(failure.history.back().status == OptimizationEvaluationStatus::Exception &&
                failure.last_failed_parameters[0] == .75 &&
                failure.last_error == "invalid circuit operating point",
            "failure details");
    failure = minimize_bounded({{0., 1., 0.}}, [](const auto &) {
        return std::numeric_limits<double>::infinity();
    });
    require(failure.stop == OptimizationStop::ObjectiveFailure && !failure.has_finite_point &&
                failure.history[0].status == OptimizationEvaluationStatus::NonFinite,
            "nonfinite initial");
    failure = minimize_bounded({{0., 1., 0.}}, [](const auto &) -> double {
        throw 17;
    });
    require(failure.stop == OptimizationStop::ObjectiveFailure && !failure.last_error.empty(),
            "nonstandard exception");
    failure = minimize_bounded({{0., 1., 0.}}, [](const auto &) -> double {
        throw std::runtime_error(std::string(700, 'x'));
    });
    require(failure.last_error.size() == 512, "bounded diagnostics");
    rejects<std::bad_alloc>([&] {
        minimize_bounded({{0., 1., 0.}}, [](const auto &) -> double {
            throw std::bad_alloc{};
        });
    });
    options = {};
    options.invalid_evaluation = OptimizationFailurePolicy::Reject;
    options.target = 1e-12;
    auto recovered = minimize_bounded(
        {{0., 1., 0.}},
        [](const auto &x) {
            if (x[0] == 0.) {
                throw std::domain_error("zero bias");
            }
            return (x[0] - .7) * (x[0] - .7);
        },
        options);
    require(recovered.stop == OptimizationStop::TargetReached && recovered.rejected_evaluations > 0,
            "explicit reject policy");
    auto nowhere = minimize_bounded(
        {{0., 1., 0.}},
        [](const auto &) {
            return std::numeric_limits<double>::quiet_NaN();
        },
        options);
    require(nowhere.stop == OptimizationStop::NoFinitePoint && !nowhere.has_finite_point &&
                nowhere.rejected_evaluations == nowhere.evaluations,
            "no finite point distinct from convergence");
    auto resolution = minimize_bounded({{1., std::nextafter(1., 2.), 1.}}, [](const auto &x) {
        return x[0];
    });
    require(resolution.stop == OptimizationStop::NumericalResolution && resolution.evaluations == 1,
            "unrepresentable poll steps");
    std::size_t calls = 0;
    auto cancel = minimize_bounded(
        {{0., 1., .5}},
        [&](const auto &x) {
            ++calls;
            return x[0];
        },
        {},
        [] {
            return true;
        });
    require(cancel.stop == OptimizationStop::Cancelled && calls == 0 && cancel.evaluations == 0,
            "cancel before objective");
    cancel = minimize_bounded(
        {{0., 1., .5}},
        [&](const auto &x) {
            ++calls;
            return x[0];
        },
        {},
        [&] {
            return calls == 3;
        });
    require(cancel.stop == OptimizationStop::Cancelled && calls == 3 && cancel.evaluations == 3,
            "cancel in poll");

    // Validation must not call the objective or silently clamp the initial point.
    for (int scenario = 0; scenario < 19; ++scenario) {
        std::vector<OptimizationParameter> p{{0., 1., .5}};
        options = {};
        switch (scenario) {
        case 0:
            p.clear();
            break;
        case 1:
            p.resize(65, p[0]);
            break;
        case 2:
            p[0].lower = 2.;
            break;
        case 3:
            p[0].initial = 2.;
            break;
        case 4:
            p[0].upper = std::numeric_limits<double>::infinity();
            break;
        case 5:
            p[0].kind = OptimizationParameterKind::LogReal;
            break;
        case 6:
            p[0].kind = OptimizationParameterKind::Integer;
            break;
        case 7:
            p[0] = {0., 1e16, 0., OptimizationParameterKind::Integer};
            break;
        case 8:
            p[0].kind = static_cast<OptimizationParameterKind>(99);
            break;
        case 9:
            options.max_evaluations = 0;
            break;
        case 10:
            options.max_evaluations = 100001;
            break;
        case 11:
            options.max_iterations = 0;
            break;
        case 12:
            options.minimum_step = 0.;
            break;
        case 13:
            options.initial_step = 2.;
            break;
        case 14:
            options.minimum_step = .5;
            break;
        case 15:
            options.contraction = 1.;
            break;
        case 16:
            options.target = std::numeric_limits<double>::quiet_NaN();
            break;
        case 17:
            options.invalid_evaluation = static_cast<OptimizationFailurePolicy>(99);
            break;
        case 18:
            p[0] = {-1e308, 1e308, 0.};
            break;
        }
        bool invoked = false;
        rejects<std::invalid_argument>([&] {
            minimize_bounded(
                p,
                [&](const auto &) {
                    invoked = true;
                    return 0.;
                },
                options);
        });
        require(!invoked, "invalid request invoked objective");
    }
    rejects<std::invalid_argument>([] {
        minimize_bounded({{0., 1., 0.}}, {});
    });

    // RF design: shunt C followed by series L matches a 25-ohm load to 50 ohms.
    // At 1 GHz, Zseries=25+j25, so Yinput=1/(25+j25)+j*omega*C=.02 S.
    const double omega = 2. * std::acos(-1.) * 1e9;
    const auto reflection = [](const std::vector<double> &x) {
        IdealRLCModel capacitor(
            "C", IdealElement::Capacitor, LumpedConnection::ShuntAdmittance, x[1]);
        IdealRLCModel inductor(
            "L", IdealElement::Inductor, LumpedConnection::SeriesImpedance, x[0]);
        LinearNetwork net;
        net.add(capacitor.s_parameters(1e9));
        net.add(inductor.s_parameters(1e9));
        net.connect(1, 2);
        net.terminate(3, -1. / 3.);
        return std::norm(net.external_s({0})(0, 0));
    };
    options = {};
    options.target = 1e-14;
    options.minimum_step = 1e-9;
    auto match = minimize_bounded({{1e-10, 3e-8, 2e-9, OptimizationParameterKind::LogReal},
                                   {1e-13, 1e-10, 1e-11, OptimizationParameterKind::LogReal}},
                                  reflection,
                                  options);
    require(match.stop == OptimizationStop::TargetReached && match.value < 1e-14,
            "RF matching target");
    require(std::abs(match.parameters[0] / (25. / omega) - 1.) < 1e-6 &&
                std::abs(match.parameters[1] / (.02 / omega) - 1.) < 1e-6,
            "analytic L/C values");
    const Complex physical = 1. / (1. / Complex{25., omega * match.parameters[0]} +
                                   Complex{0., omega * match.parameters[1]});
    require(std::abs(physical - Complex{50., 0.}) < 2e-5, "independent physical admittance");
    require(std::strcmp(optimization_stop_name(match.stop), "target_reached") == 0, "status name");
}
