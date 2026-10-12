#include "rfmodel/monte_carlo.hpp"
#include "test_support.hpp"
using namespace rfmodel;
using D = MonteCarloDistribution;
const auto identity = [](const std::vector<double> &x) {
    return x;
};

static MonteCarloResult sequence(const std::vector<double> &values) {
    MonteCarloOptions o;
    o.samples = values.size();
    o.quantile_levels = {0., .25, .5, .75, 1.};
    std::size_t i = 0;
    return run_monte_carlo(
        {{D::Fixed, 0., 0.}},
        1,
        [&](const auto &) {
            return std::vector<double>{values[i++]};
        },
        o);
}

int main() {
    MonteCarloOptions o;
    o.samples = 10000;
    o.seed = 5489;
    auto r = run_monte_carlo({{D::Uniform, 0., 1.}}, 1, identity, o);
    require(r.history.back().values[0] ==
                static_cast<double>(9981545732273789042ULL >> 11) * 0x1p-53,
            "standard MT19937-64 golden sequence");
    require(r.random_words == o.samples && r.accepted_samples == o.samples &&
                r.stop == MonteCarloStop::Completed,
            "golden counters");
    const std::vector<MonteCarloParameter> all{{D::Fixed, 3., 0.},
                                               {D::Uniform, -1., 2.},
                                               {D::LogUniform, .1, 10.},
                                               {D::Normal, 5., 2.},
                                               {D::LogNormal, 0., .5},
                                               {D::DiscreteUniform, -3., 7.}};
    o.samples = 128;
    o.seed = 77;
    r = run_monte_carlo(all, 6, identity, o);
    auto replay = run_monte_carlo(all, 6, identity, o);
    require(r.random_words >= 7 * o.samples && r.random_words == replay.random_words,
            "word consumption");
    for (std::size_t i = 0; i < o.samples; ++i) {
        require(r.history[i].parameters == replay.history[i].parameters, "exact replay");
    }
    o.record_history = false;
    replay = run_monte_carlo(all, 6, identity, o);
    for (std::size_t j = 0; j < 6; ++j) {
        require(r.statistics[j].mean == replay.statistics[j].mean &&
                    r.statistics[j].quantiles == replay.statistics[j].quantiles &&
                    r.statistics[j].sample_variance == replay.statistics[j].sample_variance,
                "history independent statistics");
    }
    require(replay.history.empty() && replay.random_words == r.random_words,
            "history independent random stream");
    ++o.seed;
    require(run_monte_carlo(all, 6, identity, o).statistics[1].mean != r.statistics[1].mean,
            "seed changes stream");
    o = {};
    o.samples = 3;
    r = run_monte_carlo({{D::Fixed, 2., 0.},
                         {D::Uniform, 2., 2.},
                         {D::LogUniform, 2., 2.},
                         {D::Normal, 2., 0.},
                         {D::LogNormal, 0., 0.},
                         {D::DiscreteUniform, 2., 2.}},
                        6,
                        identity,
                        o);
    require(r.random_words == 0 && r.statistics[4].mean == 1.,
            "degenerate distributions draw nothing");
    auto s = sequence({1., 2., 3., 4.}).statistics[0];
    near(s.mean, 2.5);
    near(*s.sample_variance, 5. / 3.);
    near(*s.standard_error, std::sqrt(5. / 12.));
    require(s.quantiles == std::vector<double>({1., 1.75, 2.5, 3.25, 4.}),
            "type 7 exact quantiles");
    s = sequence({7.}).statistics[0];
    require(!s.sample_variance && !s.sample_standard_deviation && !s.standard_error && s.mean == 7.,
            "one sample variance undefined");
    const double a = 1e150, b = std::nextafter(a, std::numeric_limits<double>::infinity()),
                 delta = b - a;
    s = sequence({a, b}).statistics[0];
    require(std::abs(*s.sample_variance / (delta * delta / 2.) - 1.) < 1e-14,
            "large offset adjacent ULP variance");
    const double max = std::numeric_limits<double>::max();
    s = sequence({-max, max}).statistics[0];
    require(s.mean == 0. && !s.sample_variance && !s.sample_standard_deviation &&
                s.standard_error && *s.standard_error == max && s.quantiles[2] == 0.,
            "extreme finite observations retained");
    s = sequence({max, max}).statistics[0];
    require(s.mean == max && *s.sample_variance == 0., "extreme constant statistics");
    const double denorm = std::numeric_limits<double>::denorm_min();
    s = sequence({0., 4. * denorm}).statistics[0];
    require(s.mean == 2. * denorm && *s.standard_error == 2. * denorm, "subnormal statistics");
    o = {};
    o.samples = 50000;
    o.record_history = false;
    o.seed = 20261012;
    r = run_monte_carlo(all, 6, identity, o);
    const std::vector<double> means{3., .5, 9.9 / std::log(100.), 5., std::exp(.125), 2.};
    const std::vector<double> variances{0.,
                                        .75,
                                        (100. - .01) / (2. * std::log(100.)) - means[2] * means[2],
                                        4.,
                                        (std::exp(.25) - 1.) * std::exp(.25),
                                        10.};
    for (std::size_t j = 0; j < 6; ++j) {
        require(std::abs(r.statistics[j].mean - means[j]) <=
                    6. * std::sqrt(variances[j] / static_cast<double>(o.samples)),
                "analytic distribution mean within six SE");
        require(std::abs(*r.statistics[j].sample_variance - variances[j]) <= .1 * variances[j],
                "analytic distribution variance within ten percent");
    }
    o.record_history = true;
    o.samples = 12000;
    r = run_monte_carlo({{D::DiscreteUniform, -3., 7.}}, 1, identity, o);
    std::vector<std::size_t> bins(11);
    for (const auto &e : r.history) {
        require(e.values[0] == std::trunc(e.values[0]) && e.values[0] >= -3. && e.values[0] <= 7.,
                "integer support");
        ++bins[static_cast<std::size_t>(e.values[0] + 3.)];
    }
    for (auto count : bins) {
        require(std::abs(static_cast<double>(count) - 12000. / 11.) <
                    6. * std::sqrt(12000. * 10. / 121.),
                "unbiased discrete histogram");
    }
    o = {};
    o.samples = 4;
    o.criteria = {{{0.}, {2.}}};
    std::size_t called = 0;
    auto failing = [&](const auto &) {
        ++called;
        if (called == 2) {
            throw std::runtime_error("invalid device");
        }
        return std::vector<double>{static_cast<double>(called - 1)};
    };
    r = run_monte_carlo({{D::Uniform, 0., 1.}}, 1, failing, o);
    require(r.stop == MonteCarloStop::EvaluationFailure && r.samples_attempted == 2 &&
                r.accepted_samples == 1 && r.rejected_samples == 1 && r.random_words == 2 &&
                r.passed_samples == 1 && *r.passed_fraction == .5 && *r.valid_pass_fraction == 1.,
            "stop and distinct denominators");
    o.invalid_sample = MonteCarloFailurePolicy::Reject;
    called = 0;
    r = run_monte_carlo({{D::Uniform, 0., 1.}}, 1, failing, o);
    require(r.stop == MonteCarloStop::Completed && called == 4 && r.samples_attempted == 4 &&
                r.accepted_samples == 3 && r.rejected_samples == 1 && r.passed_samples == 2 &&
                *r.passed_fraction == .5 && *r.valid_pass_fraction == 2. / 3.,
            "reject consumes attempts without replacement");
    near(r.statistics[0].mean, 5. / 3.);
    r = run_monte_carlo(
        {{D::Fixed, 0., 0.}},
        1,
        [](const auto &) {
            return std::vector<double>{};
        },
        o);
    require(r.stop == MonteCarloStop::Completed && !r.has_statistics && r.evaluations == 4 &&
                r.rejected_samples == 4 && r.statistics[0].count == 0 &&
                std::isnan(r.statistics[0].mean) && r.statistics[0].quantiles.empty() &&
                !r.valid_pass_fraction && *r.passed_fraction == 0. &&
                r.history[0].status == MonteCarloSampleStatus::InvalidMeasurementCount,
            "all rejected empty statistics");
    o.invalid_sample = MonteCarloFailurePolicy::Stop;
    r = run_monte_carlo(
        {{D::Fixed, 0., 0.}},
        1,
        [](const auto &) {
            return std::vector<double>{std::numeric_limits<double>::quiet_NaN()};
        },
        o);
    require(r.history[0].status == MonteCarloSampleStatus::NonFiniteMeasurement &&
                r.history[0].values.empty(),
            "nonfinite measurement omitted");
    called = 0;
    r = run_monte_carlo({{D::LogNormal, 10000., .01}, {D::Uniform, 0., 1.}}, 1, failing, o);
    require(called == 0 && r.evaluations == 0 && r.samples_attempted == 1 && r.random_words == 3 &&
                r.stop == MonteCarloStop::SamplingFailure,
            "overflow consumes complete vector but does not call evaluator");
    r = run_monte_carlo({{D::LogNormal, -10000., .01}}, 1, identity, o);
    require(r.stop == MonteCarloStop::SamplingFailure, "lognormal underflow failure");
    r = run_monte_carlo({{D::Uniform, -max, max}}, 1, identity, o);
    require(r.accepted_samples == 4 && std::isfinite(r.statistics[0].mean),
            "extreme uniform bounds");
    called = 0;
    r = run_monte_carlo({{D::Uniform, 0., 1.}}, 1, failing, o, [] {
        return true;
    });
    require(r.stop == MonteCarloStop::Cancelled && r.random_words == 0 &&
                r.samples_attempted == 0 && called == 0 && !r.passed_fraction &&
                !r.valid_pass_fraction,
            "cancel before sampling");
    called = 0;
    r = run_monte_carlo(
        {{D::Uniform, 0., 1.}},
        1,
        [&](const auto &x) {
            ++called;
            return x;
        },
        o,
        [&] {
            return called == 2;
        });
    require(r.stop == MonteCarloStop::Cancelled && r.accepted_samples == 2 && r.random_words == 2,
            "cancel between callbacks");
    rejects<std::bad_alloc>([&] {
        run_monte_carlo(
            {{D::Fixed, 0., 0.}},
            1,
            [](const auto &) -> std::vector<double> {
                throw std::bad_alloc();
            },
            o);
    });
    rejects<std::runtime_error>([&] {
        run_monte_carlo({{D::Fixed, 0., 0.}}, 1, identity, o, []() -> bool {
            throw std::runtime_error("cancel failure");
        });
    });
    r = run_monte_carlo(
        {{D::Fixed, 0., 0.}},
        1,
        [](const auto &) -> std::vector<double> {
            throw 42;
        },
        o);
    require(r.history[0].status == MonteCarloSampleStatus::Exception,
            "nonstandard exception captured");
    r = run_monte_carlo(
        {{D::Fixed, 0., 0.}},
        1,
        [](const auto &) -> std::vector<double> {
            throw std::runtime_error(std::string(1000, 'x'));
        },
        o);
    require(r.last_error.size() == 512, "bounded diagnostic");
    std::vector<MonteCarloParameter> mutable_parameters{{D::Fixed, 7., 0.}};
    o = {};
    o.samples = 3;
    r = run_monte_carlo(
        mutable_parameters,
        1,
        [&](const auto &x) {
            mutable_parameters[0].first = 0.;
            o.samples = 1;
            o.quantile_levels.clear();
            return x;
        },
        o);
    require(r.accepted_samples == 3 && r.statistics[0].mean == 7. && r.quantile_levels.size() == 3,
            "frozen request");
    o = {};
    o.samples = 2;
    o.criteria = {{{1.}, {1.}}, {{}, {2.}}};
    r = run_monte_carlo({{D::Fixed, 1., 0.}, {D::Fixed, 2., 0.}}, 2, identity, o);
    require(r.passed_samples == 2, "inclusive multi-metric limits");
    o.criteria[1].upper = 1.;
    require(
        run_monte_carlo({{D::Fixed, 1., 0.}, {D::Fixed, 2., 0.}}, 2, identity, o).passed_samples ==
            0,
        "all limits must pass");
    o = {};
    o.quantile_levels.clear();
    r = run_monte_carlo({{D::Fixed, 1., 0.}}, 1, identity, o);
    require(r.statistics[0].quantiles.empty() && !r.passed_fraction && !r.valid_pass_fraction,
            "optional outputs disabled");
    for (const auto &p : std::vector<MonteCarloParameter>{
             {D::Fixed, 1., 2.},
             {D::Uniform, 2., 1.},
             {D::LogUniform, 0., 1.},
             {D::Normal, 0., -1.},
             {D::LogNormal, 10000., 0.},
             {D::DiscreteUniform, .5, 2.},
             {D::DiscreteUniform, 0., 9007199254740992.},
             {static_cast<D>(99), 0., 0.},
             {D::Normal, max, std::numeric_limits<double>::infinity()}}) {
        rejects<std::invalid_argument>([&] {
            run_monte_carlo({p}, 1, identity);
        });
    }
    rejects<std::invalid_argument>([&] {
        run_monte_carlo({}, 1, identity);
    });
    rejects<std::invalid_argument>([&] {
        run_monte_carlo({{D::Fixed, 0., 0.}}, 0, identity);
    });
    rejects<std::invalid_argument>([&] {
        run_monte_carlo({{D::Fixed, 0., 0.}}, 1, {});
    });
    auto invalid = [&](const MonteCarloOptions &bad) {
        rejects<std::invalid_argument>([&] {
            run_monte_carlo({{D::Fixed, 0., 0.}}, 1, identity, bad);
        });
    };
    o = {};
    o.samples = 0;
    invalid(o);
    o.samples = 100001;
    invalid(o);
    o = {};
    o.quantile_levels = {.5, .5};
    invalid(o);
    o.quantile_levels = {-1.};
    invalid(o);
    o.quantile_levels = {std::numeric_limits<double>::quiet_NaN()};
    invalid(o);
    o = {};
    o.criteria = {{}};
    invalid(o);
    o.criteria = {{{2.}, {1.}}};
    invalid(o);
    o.criteria = {{{0.}, {}}, {{}, {}}};
    invalid(o);
    o = {};
    o.invalid_sample = static_cast<MonteCarloFailurePolicy>(99);
    invalid(o);
    rejects<std::invalid_argument>([] {
        monte_carlo_stop_name(static_cast<MonteCarloStop>(99));
    });
}
