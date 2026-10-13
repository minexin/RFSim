#include "rfmodel/analysis/monte_carlo.hpp"
#include "../support/test_support.hpp"
using namespace rfmodel;
using D = MonteCarloDistribution;
const auto identity_joint = [](const std::vector<double> &x) {
    return x;
};

int main() {
    MonteCarloOptions o;
    o.samples = 5;
    o.joint_statistics = true;
    std::size_t index = 0;
    const std::vector<std::vector<double>> points{
        {1., 9., 7.}, {3., 2., 7.}, {2., 4., 7.}, {5., -1., 7.}, {4., 6., 7.}};
    auto r = run_monte_carlo(
        {{D::Fixed, 1., 0.}},
        3,
        [&](const auto &) {
            return points[index++];
        },
        o);
    require(r.metric_joint.count == 5 && r.metric_joint.dimension == 3, "joint dimensions");
    // Independently hand-computed sums: means 3,4,7; centered products -18.
    near(*r.metric_joint.sample_covariance[0], 2.5);
    near(*r.metric_joint.sample_covariance[1], -4.5);
    near(*r.metric_joint.sample_covariance[4], 14.5);
    near(*r.metric_joint.sample_correlation[1], -4.5 / std::sqrt(2.5 * 14.5));
    require(r.metric_joint.sample_covariance[2] == 0. && !r.metric_joint.sample_correlation[2] &&
                !r.metric_joint.sample_correlation[8] &&
                r.parameter_joint.sample_covariance[0] == 0.,
            "constant covariance and undefined correlation");
    require(r.metric_joint.sample_covariance[1] == r.metric_joint.sample_covariance[3],
            "exact symmetry");
    for (std::size_t j = 0; j < 3; ++j) {
        require(r.metric_joint.sample_covariance[j * 3 + j] == r.statistics[j].sample_variance,
                "diagonal matches marginal variance");
    }
    const double huge = std::numeric_limits<double>::max();
    index = 0;
    o.samples = 2;
    r = run_monte_carlo(
        {{D::Fixed, 0., 0.}},
        2,
        [&](const auto &) {
            const double sign = index++ ? 1. : -1.;
            return std::vector<double>{sign * huge, -sign * 1e-308};
        },
        o);
    require(!r.metric_joint.sample_covariance[0], "variance overflow is unavailable");
    require(std::abs(*r.metric_joint.sample_covariance[1] / (-2. * (huge * 1e-308)) - 1.) < 1e-14,
            "cross covariance avoids intermediate overflow");
    near(*r.metric_joint.sample_correlation[1], -1.);
    // Covariance underflows independently of a well-defined correlation.
    require(r.metric_joint.sample_covariance[3] == 0. && r.metric_joint.sample_correlation[3] == 1.,
            "tiny variance retains correlation");
    o.samples = 1;
    r = run_monte_carlo({{D::Fixed, 1., 0.}}, 1, identity_joint, o);
    require(r.metric_joint.count == 1 && !r.metric_joint.sample_covariance[0],
            "one sample unavailable");
    r = run_monte_carlo({{D::Fixed, 1., 0.}}, 1, identity_joint, o, [] {
        return true;
    });
    require(r.metric_joint.count == 0 && r.metric_joint.dimension == 1 &&
                !r.metric_joint.sample_covariance[0],
            "empty cancelled joint statistics");
    o = {};
    o.samples = 128;
    o.seed = 20261012;
    o.joint_statistics = true;
    o.latent_correlation = {1., .6, 0., .6, 1., .8, 0., .8, 1.};
    const std::vector<MonteCarloParameter> normals(3, {D::Normal, 0., 1.});
    r = run_monte_carlo(normals, 3, identity_joint, o);
    require(r.algorithm == monte_carlo_copula_algorithm && r.latent_rank == 2 &&
                r.random_words == 6 * o.samples,
            "pivoted rank and full-vector random budget");
    MonteCarloOptions independent = o;
    independent.latent_correlation.clear();
    const auto basis = run_monte_carlo(normals, 3, identity_joint, independent);
    for (std::size_t i = 0; i < o.samples; ++i) {
        const auto &x = r.history[i].parameters;
        const auto &z = basis.history[i].parameters;
        near(x[0], z[0]);
        near(x[2], z[1]);
        near(x[1], .6 * z[0] + .8 * z[1]);
    }
    for (std::size_t i = 0; i < 9; ++i) {
        near(r.effective_latent_correlation[i], o.latent_correlation[i]);
    }
    auto replay = run_monte_carlo(normals, 3, identity_joint, o);
    for (std::size_t i = 0; i < o.samples; ++i) {
        require(r.history[i].parameters == replay.history[i].parameters, "copula replay");
    }
    o.record_history = false;
    replay = run_monte_carlo(normals, 3, identity_joint, o);
    require(replay.metric_joint.sample_covariance == r.metric_joint.sample_covariance &&
                replay.parameter_joint.sample_correlation == r.parameter_joint.sample_correlation &&
                replay.random_words == r.random_words,
            "history independent joint moments");
    // Identity copula preserves the direct normal stream for equal parameters.
    o.latent_correlation = {1., 0., 0., 0., 1., 0., 0., 0., 1.};
    o.record_history = true;
    r = run_monte_carlo(normals, 3, identity_joint, o);
    for (std::size_t i = 0; i < o.samples; ++i) {
        require(r.history[i].parameters == basis.history[i].parameters, "identity normal stream");
    }
    // All six marginal transforms share one latent variate; last is anticorrelated.
    std::vector<MonteCarloParameter> all{{D::Normal, 0., 1.},
                                         {D::Uniform, -2., 3.},
                                         {D::LogUniform, 1., 10.},
                                         {D::LogNormal, .1, .2},
                                         {D::Fixed, 7., 0.},
                                         {D::DiscreteUniform, -3., 7.}};
    o.latent_correlation.assign(36, 1.);
    for (std::size_t j = 0; j < 5; ++j) {
        o.latent_correlation[5 * 6 + j] = o.latent_correlation[j * 6 + 5] = -1.;
    }
    r = run_monte_carlo(all, 6, identity_joint, o);
    require(r.latent_rank == 1 && r.random_words == 12 * o.samples, "singular mixed copula budget");
    for (const auto &e : r.history) {
        const auto &x = e.parameters;
        const double u = .5 * (1. + std::erf(x[0] / std::sqrt(2.)));
        near(x[1], -2. + 5. * u);
        near(x[2], std::pow(10., u));
        near(x[3], std::exp(.1 + .2 * x[0]));
        require(x[4] == 7. && x[5] == -3. + std::min(10., std::floor((1. - u) * 11.)),
                "fixed and discrete inverse marginal");
    }
    // Known Gaussian and lognormal population covariance, with preset tolerances.
    o = {};
    o.samples = 50000;
    o.seed = 20261012;
    o.record_history = false;
    o.joint_statistics = true;
    o.latent_correlation = {1., -.7, -.7, 1.};
    r = run_monte_carlo({{D::Normal, 3., 2.}, {D::Normal, -5., 4.}}, 2, identity_joint, o);
    const double cov = -.7 * 2. * 4., se = std::sqrt((4. * 16. + cov * cov) / 49999.);
    require(std::abs(*r.metric_joint.sample_covariance[1] - cov) < 6. * se,
            "normal covariance within six SE");
    require(std::abs(r.statistics[0].mean - 3.) < 12. / std::sqrt(50000.), "normal mean");
    o.latent_correlation = {1., .7, .7, 1.};
    r = run_monte_carlo({{D::LogNormal, .1, .3}, {D::LogNormal, -.2, .4}}, 2, identity_joint, o);
    const double log_cov = std::exp(.1 - .2 + (.09 + .16) / 2.) * std::expm1(.7 * .3 * .4);
    require(std::abs(*r.metric_joint.sample_covariance[1] / log_cov - 1.) < .05,
            "lognormal covariance analytic reference");
    r = run_monte_carlo({{D::Uniform, 0., 1.}, {D::Uniform, 0., 1.}}, 2, identity_joint, o);
    const double uniform_corr = 6. / std::acos(-1.) * std::asin(.7 / 2.);
    require(std::abs(*r.metric_joint.sample_correlation[1] - uniform_corr) < .02,
            "uniform Pearson differs from latent correlation");
    require(std::abs(r.statistics[0].mean - .5) < 6. / std::sqrt(12. * 50000.),
            "uniform marginal mean");
    // Rejections condition both joint matrices on identical accepted rows.
    o.samples = 20;
    o.record_history = true;
    o.invalid_sample = MonteCarloFailurePolicy::Reject;
    std::size_t calls = 0;
    r = run_monte_carlo(
        {{D::Normal, 0., 1.}, {D::Normal, 0., 1.}},
        2,
        [&](const auto &x) {
            if (++calls % 2) {
                throw std::runtime_error("reject odd rows");
            }
            return x;
        },
        o);
    require(r.accepted_samples == 10 && r.metric_joint.count == 10 &&
                r.parameter_joint.count == 10 &&
                r.metric_joint.sample_covariance == r.parameter_joint.sample_covariance,
            "accepted row alignment");
    double mx = 0., my = 0., cross = 0.;
    for (const auto &e : r.history) {
        if (e.status == MonteCarloSampleStatus::Accepted) {
            mx += e.parameters[0] / 10.;
            my += e.parameters[1] / 10.;
        }
    }
    for (const auto &e : r.history) {
        if (e.status == MonteCarloSampleStatus::Accepted) {
            cross += (e.parameters[0] - mx) * (e.parameters[1] - my) / 9.;
        }
    }
    near(*r.parameter_joint.sample_covariance[1], cross);
    o.invalid_sample = MonteCarloFailurePolicy::Stop;
    r = run_monte_carlo({{D::LogNormal, 10000., .1}, {D::Normal, 0., 1.}}, 2, identity_joint, o);
    require(r.stop == MonteCarloStop::SamplingFailure && r.random_words == 4 &&
                r.evaluations == 0 && r.parameter_joint.count == 0,
            "full copula vector failure");
    r = run_monte_carlo({{D::Normal, 0., 1.}, {D::Normal, 0., 1.}}, 2, identity_joint, o, [] {
        return true;
    });
    require(r.random_words == 0 && r.latent_rank == 2 && r.metric_joint.count == 0,
            "correlation validated before cancellation");
    // Request is frozen before callback mutates external options.
    o.samples = 3;
    calls = 0;
    r = run_monte_carlo(
        {{D::Normal, 0., 1.}, {D::Normal, 0., 1.}},
        2,
        [&](const auto &x) {
            ++calls;
            o.latent_correlation.clear();
            o.joint_statistics = false;
            return x;
        },
        o);
    require(calls == 3 && r.metric_joint.count == 3 && r.latent_rank == 2, "frozen copula request");
    // Validate all matrix errors before evaluator or cancellation side effects.
    for (const auto &bad : std::vector<std::vector<double>>{{1.},
                                                            {1., .2, .3, 1.},
                                                            {2., 0., 0., 1.},
                                                            {1., 1.1, 1.1, 1.},
                                                            {1., NAN, NAN, 1.},
                                                            {1., INFINITY, INFINITY, 1.}}) {
        o.latent_correlation = bad;
        calls = 0;
        rejects<std::invalid_argument>([&] {
            run_monte_carlo(
                {{D::Normal, 0., 1.}, {D::Normal, 0., 1.}},
                2,
                [&](const auto &x) {
                    ++calls;
                    return x;
                },
                o,
                [&] {
                    ++calls;
                    return false;
                });
        });
        require(calls == 0, "invalid request side-effect-free");
    }
    o.latent_correlation = {1., .9, .9, .9, 1., -.9, .9, -.9, 1.};
    rejects<std::invalid_argument>([&] {
        run_monte_carlo(normals, 3, identity_joint, o);
    });
    o.latent_correlation = {1., 1., 0., 1., 1., .01, 0., .01, 1.};
    rejects<std::invalid_argument>([&] {
        run_monte_carlo(normals, 3, identity_joint, o);
    });
    o.latent_correlation = {1., 1. - 1e-15, 1. - 1e-15, 1.};
    o.samples = 2;
    r = run_monte_carlo({{D::Normal, 0., 1.}, {D::Normal, 0., 1.}}, 2, identity_joint, o);
    require(r.latent_rank == 1 && r.effective_latent_correlation[1] == 1.,
            "observable roundoff adjustment");
    // Maximum dimension singular PSD (rank one) and independent optionality.
    o.latent_correlation.assign(64 * 64, 1.);
    o.joint_statistics = true;
    r = run_monte_carlo(
        std::vector<MonteCarloParameter>(64, {D::Normal, 0., 1.}), 64, identity_joint, o);
    require(r.latent_rank == 1 && r.random_words == 256 && r.metric_joint.dimension == 64,
            "maximum dimensions");
    o = {};
    o.samples = 3;
    r = run_monte_carlo({{D::Normal, 0., 1.}}, 1, identity_joint, o);
    require(r.algorithm == monte_carlo_random_algorithm && r.latent_rank == 0 &&
                r.effective_latent_correlation.empty() && r.metric_joint.sample_covariance.empty(),
            "legacy optionality");
}
