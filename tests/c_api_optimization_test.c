#include "rfmodel/c_api.h"
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
    rfmodel_optimization_parameter *parameter;
    rfmodel_optimization_options *options;
} context;

static int
objective(const double *x, size_t n, void *data, double *value, char *error, size_t capacity) {
    context *c = (context *)data;
    ++c->calls;
    if (n != 1 || capacity != 513) {
        return 1;
    }
    if (c->mode == 1) {
        memset(error, 'x', capacity);
        return 1;
    }
    if (c->mode == 2) {
        *value = NAN;
        return 0;
    }
    if (c->mode == 3) { /* Nested API failures must not leak into outer success. */
        rfmodel_network *network = NULL;
        if (rfmodel_network_create(50., &network)) {
            return 1;
        }
        rfmodel_network_destroy(network);
        rfmodel_network_create(50., NULL);
        c->parameter->lower = 100.;
        c->options->max_evaluations = 1;
    }
    *value = (x[0] - .25) * (x[0] - .25);
    return 0;
}

static int cancelled(void *data) {
    return ((context *)data)->cancel;
}

int main(void) {
    rfmodel_optimization_parameter p = {0., 1., .5, RFMODEL_OPT_REAL};
    const rfmodel_optimization_options defaults = {10000, 2000, .25, 1e-7, .5, 0., 1, 0, 1};
    rfmodel_optimization_options o = defaults;
    rfmodel_optimization_result *r = NULL;
    rfmodel_optimization_summary s;
    rfmodel_optimization_evaluation info;
    context c = {0, 0, 0, &p, &o};
    double best[2] = {99., 88.};
    char error[514];
    CHECK(rfmodel_optimize_bounded(&p, 1, &o, objective, NULL, &c, &r) == RFMODEL_OK);
    CHECK(c.calls == 3 && rfmodel_optimization_get_summary(r, &s) == RFMODEL_OK);
    CHECK(s.stop == RFMODEL_OPT_TARGET_REACHED && s.evaluations == 3 && s.history_count == 3 &&
          s.has_finite_point);
    CHECK(rfmodel_optimization_get_parameters(r, best, 2) == RFMODEL_OK && best[0] == .25 &&
          best[1] == 88.);
    CHECK(rfmodel_optimization_get_last_failure(r, NULL, 0, error, 1) == RFMODEL_OK && !error[0]);
    for (size_t i = 0; i < 3; ++i) {
        const double expected[3] = {.5, .75, .25};
        CHECK(rfmodel_optimization_get_evaluation(r, i, &info, NULL, 0, NULL, 0) == RFMODEL_OK);
        CHECK(info.status == RFMODEL_OPT_FINITE && info.error_bytes == 0);
        memset(error, 'z', sizeof(error));
        CHECK(rfmodel_optimization_get_evaluation(r, i, &info, best, 2, error, 514) == RFMODEL_OK);
        CHECK(best[0] == expected[i] && best[1] == 88. && error[0] == 0 && error[1] == 'z');
    }
    /* Query failures are atomic, including overlap and overflowing capacities. */
    for (int scenario = 0; scenario < 8; ++scenario) {
        rfmodel_optimization_evaluation saved;
        size_t index = 0, cap = 2, ecap = 514;
        double *point = best;
        char *ep = error;
        memset(&info, 0x35, sizeof(info));
        memcpy(&saved, &info, sizeof(info));
        best[0] = 99.;
        memset(error, 'z', sizeof(error));
        switch (scenario) {
        case 0:
            index = 3;
            break;
        case 1:
            cap = 0;
            break;
        case 2:
            cap = SIZE_MAX;
            break;
        case 3:
            ecap = 0;
            break;
        case 4:
            point = (double *)&info;
            break;
        case 5:
            ep = (char *)best;
            break;
        case 6:
            point = (double *)r;
            break;
        case 7:
            point = NULL;
            break;
        }
        CHECK(rfmodel_optimization_get_evaluation(r, index, &info, point, cap, ep, ecap) !=
              RFMODEL_OK);
        CHECK(memcmp(&info, &saved, sizeof(info)) == 0 && best[0] == 99. && error[0] == 'z');
    }
    CHECK(c.calls == 3);
    CHECK(rfmodel_optimize_bounded(&p, 1, &o, objective, NULL, &c, &r) != RFMODEL_OK &&
          c.calls == 3);
    rfmodel_optimization_destroy(r);
    r = NULL;
    /* Request validation precedes callbacks and preserves the output slot. */
    for (int scenario = 0; scenario < 10; ++scenario) {
        rfmodel_optimization_parameter q = p;
        rfmodel_optimization_options options = defaults;
        size_t n = 1;
        switch (scenario) {
        case 0:
            n = 0;
            break;
        case 1:
            n = 65;
            break;
        case 2:
            q.kind = 8;
            break;
        case 3:
            q.initial = 2.;
            break;
        case 4:
            options.record_history = 2;
            break;
        case 5:
            options.max_evaluations = 0;
            break;
        case 6:
            options.contraction = 1.;
            break;
        case 7:
            options.has_target = 0;
            options.target = 1.;
            break;
        case 8:
            q.kind = RFMODEL_OPT_LOG_REAL;
            break;
        case 9:
            q.kind = RFMODEL_OPT_INTEGER;
            break;
        }
        CHECK(rfmodel_optimize_bounded(&q, n, &options, objective, NULL, &c, &r) != RFMODEL_OK &&
              !r && c.calls == 3);
    }
    CHECK(rfmodel_optimize_bounded(
              &p, 1, &o, objective, NULL, &c, (rfmodel_optimization_result **)&p) != RFMODEL_OK);
    CHECK(p.lower == 0. && p.upper == 1.);
    CHECK(rfmodel_optimize_bounded(
              &p, 1, &o, objective, NULL, &c, (rfmodel_optimization_result **)&o) != RFMODEL_OK);
    c.calls = 0;
    c.mode = 3;
    CHECK(rfmodel_optimize_bounded(&p, 1, &o, objective, NULL, &c, &r) == RFMODEL_OK &&
          !rfmodel_last_error()[0]);
    CHECK(c.calls == 3); /* Original input mutation cannot alter the copied request. */
    rfmodel_optimization_destroy(r);
    r = NULL;
    p.lower = 0.;
    o = defaults;
    c.mode = 1;
    c.calls = 0;
    CHECK(rfmodel_optimize_bounded(&p, 1, NULL, objective, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_optimization_get_summary(r, &s) == RFMODEL_OK &&
          s.stop == RFMODEL_OPT_OBJECTIVE_FAILURE && !s.has_finite_point &&
          s.last_error_bytes == 512);
    CHECK(rfmodel_optimization_get_last_failure(r, best, 2, error, 514) == RFMODEL_OK &&
          best[0] == .5 && strlen(error) == 512);
    CHECK(rfmodel_optimization_get_evaluation(r, 0, &info, NULL, 0, NULL, 0) == RFMODEL_OK &&
          info.status == RFMODEL_OPT_EXCEPTION);
    best[0] = 99.;
    error[0] = 'z';
    CHECK(rfmodel_optimization_get_last_failure(r, best, 2, error, 512) != RFMODEL_OK &&
          best[0] == 99. && error[0] == 'z');
    rfmodel_optimization_destroy(r);
    r = NULL;
    c.mode = 2;
    o.reject_invalid = 1;
    o.max_evaluations = 2;
    CHECK(rfmodel_optimize_bounded(&p, 1, &o, objective, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_optimization_get_summary(r, &s) == RFMODEL_OK &&
          s.stop == RFMODEL_OPT_EVALUATION_LIMIT && s.rejected_evaluations == 2);
    CHECK(rfmodel_optimization_get_evaluation(r, 0, &info, NULL, 0, NULL, 0) == RFMODEL_OK &&
          info.status == RFMODEL_OPT_NONFINITE);
    rfmodel_optimization_destroy(r);
    r = NULL;
    c.mode = 0;
    c.calls = 0;
    c.cancel = 1;
    CHECK(rfmodel_optimize_bounded(&p, 1, NULL, objective, cancelled, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_optimization_get_summary(r, &s) == RFMODEL_OK &&
          s.stop == RFMODEL_OPT_CANCELLED && !s.evaluations && !c.calls);
    rfmodel_optimization_destroy(r);
    r = NULL;
    c.cancel = -1;
    CHECK(rfmodel_optimize_bounded(&p, 1, NULL, objective, cancelled, &c, &r) != RFMODEL_OK && !r &&
          !c.calls);
    o = defaults;
    o.record_history = 0;
    CHECK(rfmodel_optimize_bounded(&p, 1, &o, objective, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_optimization_get_summary(r, &s) == RFMODEL_OK && s.evaluations == 3 &&
          !s.history_count);
    rfmodel_optimization_destroy(r);
    rfmodel_optimization_destroy(NULL);
    return 0;
}
