#include "rfmodel/api/c_api.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, rfmodel_last_error());                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct context {
    int calls, mode, cancel;
    rfmodel_monte_carlo_parameter *parameter;
    rfmodel_monte_carlo_options *options;
    double *quantiles;
    rfmodel_monte_carlo_limits *criteria;
} context;

static int evaluate(const double *p,
                    size_t n,
                    void *data,
                    double *v,
                    size_t capacity,
                    size_t *count,
                    char *error,
                    size_t ecap) {
    context *c = (context *)data;
    ++c->calls;
    if (n != 1 || ecap != 513 || capacity != 2 || *count != 0 || !isnan(v[0]) || !isnan(v[1])) {
        return 1;
    }
    if (c->mode == 1 || (c->mode == 7 && c->calls == 2)) {
        memset(error, 'x', ecap);
        return 1;
    }
    if (c->mode == 2) {
        *count = SIZE_MAX;
        return 0;
    }
    *count = capacity;
    if (c->mode == 3) {
        v[0] = NAN;
        v[1] = 0.;
        return 0;
    }
    if (c->mode == 4) {
        v[0] = 1.;
        return 0;
    }
    if (c->mode == 5) {
        rfmodel_network *network = NULL;
        if (rfmodel_network_create(50., &network)) {
            return 1;
        }
        rfmodel_network_destroy(network);
        rfmodel_network_create(50., NULL);
        c->parameter->first = 100.;
        c->options->samples = 1;
        c->quantiles[0] = .9;
        c->criteria[0].upper = -1.;
    }
    v[0] = p[0];
    v[1] = 2. * p[0];
    return 0;
}

static int cancel(void *data) {
    context *c = (context *)data;
    return c->cancel == 2 ? c->calls >= 2 : c->cancel;
}

int main(void) {
    rfmodel_monte_carlo_parameter p = {RFMODEL_MC_UNIFORM, 0., 1.};
    double q[3] = {0., .5, 1.};
    rfmodel_monte_carlo_limits limits[2] = {{0, 1, 0., 1.}, {1, 0, 0., 0.}};
    const rfmodel_monte_carlo_options defaults = {4, UINT64_MAX, 0, 1, q, 3, limits, 2};
    rfmodel_monte_carlo_options o = defaults;
    rfmodel_monte_carlo_result *r = NULL;
    rfmodel_monte_carlo_summary s;
    rfmodel_monte_carlo_statistics stats;
    rfmodel_monte_carlo_sample info;
    context c = {0, 0, 0, &p, &o, q, limits};
    double point[2] = {99., 88.}, values[3] = {99., 99., 88.}, quantiles[4] = {99., 99., 99., 88.};
    char error[514];
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && c.calls == 4 &&
          s.seed == UINT64_MAX && s.random_words == 4 && s.parameter_count == 1 &&
          s.metric_count == 2 && s.history_count == 4 && s.accepted_samples == 4 &&
          s.passed_samples == 4 && s.passed_fraction == 1. && s.valid_pass_fraction == 1.);
    CHECK(rfmodel_monte_carlo_get_quantile_levels(r, quantiles, 4) == RFMODEL_OK &&
          quantiles[0] == 0. && quantiles[1] == .5 && quantiles[2] == 1. && quantiles[3] == 88.);
    double sum = 0., observations[4];
    for (size_t i = 0; i < 4; ++i) {
        CHECK(rfmodel_monte_carlo_get_sample(r, i, &info, NULL, 0, NULL, 0, NULL, 0) ==
                  RFMODEL_OK &&
              info.value_count == 2 && !info.error_bytes && info.passed &&
              info.status == RFMODEL_MC_ACCEPTED);
        memset(error, 'z', sizeof(error));
        CHECK(rfmodel_monte_carlo_get_sample(
                  r, i, &info, point, 2, values, 3, error, sizeof(error)) == RFMODEL_OK);
        CHECK(values[0] == point[0] && values[1] == 2. * point[0] && point[1] == 88. &&
              values[2] == 88. && error[0] == 0 && error[1] == 'z');
        observations[i] = point[0];
        sum += point[0];
    }
    CHECK(rfmodel_monte_carlo_get_statistics(r, 0, &stats, NULL, 0) == RFMODEL_OK &&
          stats.count == 4 && stats.quantile_count == 3);
    double ss = 0.;
    for (size_t i = 0; i < 4; ++i) {
        ss += (observations[i] - sum / 4.) * (observations[i] - sum / 4.);
    }
    CHECK(fabs(stats.mean - sum / 4.) < 1e-14 && fabs(stats.sample_variance - ss / 3.) < 1e-14 &&
          stats.has_sample_variance && stats.has_sample_standard_deviation &&
          stats.has_standard_error);
    CHECK(rfmodel_monte_carlo_get_statistics(r, 0, &stats, quantiles, 4) == RFMODEL_OK &&
          quantiles[3] == 88.);
    CHECK(rfmodel_monte_carlo_get_last_failure(r, NULL, 0, error, 1) == RFMODEL_OK && !error[0]);
    /* Every read must validate every output before committing any writes. */
    for (int scenario = 0; scenario < 12; ++scenario) {
        rfmodel_monte_carlo_sample saved;
        double *pp = point, *vv = values;
        char *ee = error;
        size_t index = 0, pc = 2, vc = 3, ec = sizeof(error);
        memset(&info, 0x35, sizeof(info));
        memcpy(&saved, &info, sizeof(info));
        point[0] = 99.;
        values[0] = 99.;
        memset(error, 'z', sizeof(error));
        switch (scenario) {
        case 0:
            index = 4;
            break;
        case 1:
            pc = 0;
            break;
        case 2:
            vc = 1;
            break;
        case 3:
            ec = 0;
            break;
        case 4:
            pc = SIZE_MAX;
            break;
        case 5:
            vc = SIZE_MAX;
            break;
        case 6:
            pp = (double *)&info;
            break;
        case 7:
            vv = point;
            break;
        case 8:
            ee = (char *)values;
            break;
        case 9:
            pp = (double *)r;
            break;
        case 10:
            pp = NULL;
            break;
        case 11:
            vv = NULL;
            break;
        }
        CHECK(rfmodel_monte_carlo_get_sample(r, index, &info, pp, pc, vv, vc, ee, ec) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(!memcmp(&saved, &info, sizeof(info)) && point[0] == 99. && values[0] == 99. &&
              error[0] == 'z');
    }
    for (int scenario = 0; scenario < 5; ++scenario) {
        rfmodel_monte_carlo_statistics saved;
        double *qq = quantiles;
        size_t cap = 4, index = 0;
        memset(&stats, 0x35, sizeof(stats));
        memcpy(&saved, &stats, sizeof(stats));
        quantiles[0] = 99.;
        switch (scenario) {
        case 0:
            index = 2;
            break;
        case 1:
            cap = 2;
            break;
        case 2:
            cap = SIZE_MAX;
            break;
        case 3:
            qq = (double *)&stats;
            break;
        case 4:
            qq = (double *)r;
            break;
        }
        CHECK(rfmodel_monte_carlo_get_statistics(r, index, &stats, qq, cap) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(!memcmp(&saved, &stats, sizeof(stats)) && quantiles[0] == 99.);
    }
    CHECK(rfmodel_monte_carlo_get_quantile_levels(r, quantiles, 2) == RFMODEL_INVALID_ARGUMENT &&
          quantiles[0] == 99.);
    CHECK(rfmodel_monte_carlo_get_summary(r, (rfmodel_monte_carlo_summary *)r) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) ==
              RFMODEL_INVALID_ARGUMENT &&
          c.calls == 4);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    for (int scenario = 0; scenario < 14; ++scenario) {
        rfmodel_monte_carlo_parameter pp = p;
        rfmodel_monte_carlo_options oo = defaults;
        rfmodel_monte_carlo_limits ll[2];
        memcpy(ll, limits, sizeof(ll));
        oo.criteria = ll;
        size_t n = 1, m = 2;
        switch (scenario) {
        case 0:
            n = 0;
            break;
        case 1:
            n = 65;
            break;
        case 2:
            m = 0;
            break;
        case 3:
            m = 65;
            break;
        case 4:
            pp.distribution = 99;
            break;
        case 5:
            pp.first = 2.;
            break;
        case 6:
            oo.record_history = 2;
            break;
        case 7:
            oo.reject_invalid = -1;
            break;
        case 8:
            oo.samples = 0;
            break;
        case 9:
            oo.quantile_count = 257;
            break;
        case 10:
            oo.quantile_levels = NULL;
            break;
        case 11:
            oo.criteria_count = 1;
            break;
        case 12:
            ll[0].has_lower = 2;
            break;
        case 13:
            ll[0].lower = 1.;
            break;
        }
        CHECK(rfmodel_run_monte_carlo(&pp, n, m, &oo, evaluate, NULL, &c, &r) ==
                  RFMODEL_INVALID_ARGUMENT &&
              !r && c.calls == 4);
    }
    CHECK(rfmodel_run_monte_carlo(
              &p, 1, 2, &o, evaluate, NULL, &c, (rfmodel_monte_carlo_result **)&p) ==
              RFMODEL_INVALID_ARGUMENT &&
          p.first == 0.);
    CHECK(rfmodel_run_monte_carlo(
              &p, 1, 2, &o, evaluate, NULL, &c, (rfmodel_monte_carlo_result **)&o) ==
              RFMODEL_INVALID_ARGUMENT &&
          o.samples == 4);
    CHECK(rfmodel_run_monte_carlo(
              &p, 1, 2, &o, evaluate, NULL, &c, (rfmodel_monte_carlo_result **)q) ==
              RFMODEL_INVALID_ARGUMENT &&
          q[0] == 0.);
    CHECK(rfmodel_run_monte_carlo(
              &p, 1, 2, &o, evaluate, NULL, &c, (rfmodel_monte_carlo_result **)limits) ==
              RFMODEL_INVALID_ARGUMENT &&
          limits[0].upper == 1.);
    c.calls = 0;
    c.mode = 5;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) == RFMODEL_OK &&
          !rfmodel_last_error()[0]);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && s.passed_samples == 4 &&
          c.calls == 4);
    CHECK(rfmodel_monte_carlo_get_quantile_levels(r, quantiles, 4) == RFMODEL_OK &&
          quantiles[0] == 0.);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    p.first = 0.;
    q[0] = 0.;
    limits[0].upper = 1.;
    o = defaults;
    for (int mode = 1; mode <= 4; ++mode) {
        c.mode = mode;
        c.calls = 0;
        CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) == RFMODEL_OK);
        CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK &&
              s.stop == RFMODEL_MC_EVALUATION_FAILURE && s.rejected_samples == 1 &&
              !s.has_statistics && c.calls == 1 && s.has_passed_fraction &&
              s.passed_fraction == 0. && !s.has_valid_pass_fraction);
        CHECK(rfmodel_monte_carlo_get_sample(
                  r, 0, &info, point, 2, NULL, 0, error, sizeof(error)) == RFMODEL_OK &&
              !info.value_count &&
              info.status == (mode == 1   ? RFMODEL_MC_EXCEPTION
                              : mode == 2 ? RFMODEL_MC_INVALID_MEASUREMENT_COUNT
                                          : RFMODEL_MC_NONFINITE_MEASUREMENT));
        CHECK(rfmodel_monte_carlo_get_statistics(r, 0, &stats, NULL, 0) == RFMODEL_OK &&
              !stats.count && isnan(stats.mean) && !stats.has_sample_variance &&
              !stats.quantile_count);
        if (mode == 1) {
            CHECK(s.last_error_bytes == 512 && strlen(error) == 512);
            point[0] = 99.;
            error[0] = 'z';
            CHECK(rfmodel_monte_carlo_get_last_failure(r, point, 2, error, 512) ==
                      RFMODEL_INVALID_ARGUMENT &&
                  point[0] == 99. && error[0] == 'z');
            CHECK(rfmodel_monte_carlo_get_last_failure(r, point, 2, error, sizeof(error)) ==
                      RFMODEL_OK &&
                  strlen(error) == 512);
        }
        rfmodel_monte_carlo_destroy(r);
        r = NULL;
    }
    c.mode = 7;
    c.calls = 0;
    o.reject_invalid = 1;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && c.calls == 4 &&
          s.accepted_samples == 3 && s.rejected_samples == 1 && s.stop == RFMODEL_MC_COMPLETED &&
          s.passed_fraction == .75 && s.valid_pass_fraction == 1.);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    c.calls = 0;
    c.mode = 0;
    p.distribution = RFMODEL_MC_LOG_NORMAL;
    p.first = 10000.;
    p.second = .01;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && !c.calls &&
          s.rejected_samples == 4 && s.random_words == 8 && !s.evaluations);
    CHECK(rfmodel_monte_carlo_get_last_failure(r, point, 2, error, sizeof(error)) == RFMODEL_OK &&
          isinf(point[0]));
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    p.distribution = RFMODEL_MC_FIXED;
    p.first = 2.;
    p.second = 0.;
    c.cancel = 1;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, NULL, evaluate, cancel, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && s.stop == RFMODEL_MC_CANCELLED &&
          !s.samples_attempted && !s.random_words && s.quantile_count == 3 &&
          s.requested_samples == 1000);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    c.cancel = -1;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, NULL, evaluate, cancel, &c, &r) ==
              RFMODEL_SOLVER_ERROR &&
          !r && !c.calls);
    c.cancel = 2;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, NULL, evaluate, cancel, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && s.stop == RFMODEL_MC_CANCELLED &&
          s.accepted_samples == 2 && !s.criteria_enabled);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    o.record_history = 0;
    o.criteria_count = 0;
    o.quantile_count = 0;
    CHECK(rfmodel_run_monte_carlo(&p, 1, 2, &o, evaluate, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_summary(r, &s) == RFMODEL_OK && s.accepted_samples == 4 &&
          !s.history_count && !s.quantile_count && !s.has_passed_fraction);
    CHECK(rfmodel_monte_carlo_get_quantile_levels(r, NULL, 0) == RFMODEL_OK);
    rfmodel_monte_carlo_destroy(r);
    rfmodel_monte_carlo_destroy(NULL);
    CHECK(rfmodel_monte_carlo_get_summary(NULL, &s) == RFMODEL_INVALID_ARGUMENT);
    return 0;
}
