#include "rfmodel/c_api.h"
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
    int calls, mode, cancelled;
    rfmodel_monte_carlo_extension *extension;
    double *matrix;
    double points[32][2];
} context;

static int evaluate(const double *p,
                    size_t n,
                    void *data,
                    double *v,
                    size_t capacity,
                    size_t *written,
                    char *error,
                    size_t error_capacity) {
    context *c = (context *)data;
    (void)error;
    (void)error_capacity;
    if (n != 2 || capacity != 3 || *written != 0) {
        return 1;
    }
    if (c->calls < 32) {
        c->points[c->calls][0] = p[0];
        c->points[c->calls][1] = p[1];
    }
    ++c->calls;
    if (c->mode == 1) {
        c->matrix[1] = c->matrix[2] = 0.;
        c->extension->joint_statistics = 0;
        c->extension->correlation_count = 0;
        rfmodel_network *network = NULL;
        if (rfmodel_network_create(50., &network)) {
            return 1;
        }
        rfmodel_network_destroy(network);
        rfmodel_network_create(50., NULL);
    }
    if (c->mode == 2 || (c->mode == 4 && c->calls % 2)) {
        return 1;
    }
    *written = 3;
    v[0] = p[0];
    v[1] = p[1];
    v[2] = p[0] + p[1];
    if (c->mode == 3) {
        const double sign = c->calls == 1 ? -1. : 1.;
        v[0] = sign * DBL_MAX;
        v[1] = -sign * 1e-308;
        v[2] = 7.;
    }
    return 0;
}

static int cancel(void *data) {
    return ((context *)data)->cancelled;
}

int main(void) {
    rfmodel_monte_carlo_parameter p[2] = {{RFMODEL_MC_NORMAL, 0., 1.}, {RFMODEL_MC_NORMAL, 0., 1.}};
    rfmodel_monte_carlo_options o = {8, 20261012, 0, 1, NULL, 0, NULL, 0};
    double matrix[4] = {1., -1., -1., 1.};
    rfmodel_monte_carlo_extension ext = {matrix, 4, 1};
    context c = {0};
    c.extension = &ext;
    c.matrix = matrix;
    c.mode = 1;
    rfmodel_monte_carlo_result *r = NULL;
    CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, &o, &ext, evaluate, NULL, &c, &r) ==
          RFMODEL_OK);
    CHECK(c.calls == 8 && rfmodel_last_error()[0] == '\0');
    rfmodel_monte_carlo_dependence dependence;
    rfmodel_monte_carlo_joint_statistics info;
    rfmodel_monte_carlo_summary summary;
    rfmodel_monte_carlo_joint_value entries[10], saved[10];
    double effective[5] = {99., 99., 99., 99., 99.};
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, NULL, 0) == RFMODEL_OK &&
          dependence.correlation_count == 4 && dependence.latent_rank == 1 &&
          dependence.joint_statistics == 1 &&
          dependence.algorithm == RFMODEL_MC_GAUSSIAN_COPULA_V1);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, effective, 5) == RFMODEL_OK &&
          effective[0] == 1. && effective[1] == -1. && effective[4] == 99.);
    memset(entries, 0x5a, sizeof(entries));
    memcpy(saved, entries, sizeof(entries));
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, entries, 10) == RFMODEL_OK &&
          info.dimension == 3 && info.count == 8);
    CHECK(memcmp(entries + 9, saved + 9, sizeof(*entries)) == 0);
    CHECK(entries[0].has_covariance && entries[0].has_correlation && entries[1].has_correlation &&
          fabs(entries[1].sample_correlation + 1.) < 1e-14);
    CHECK(entries[2].has_covariance && entries[2].sample_covariance == 0. &&
          !entries[2].has_correlation && entries[2].sample_correlation == 0.);
    CHECK(!entries[8].has_correlation && entries[8].has_covariance &&
          entries[8].sample_covariance == 0.);
    double mean = 0., variance = 0.;
    for (int i = 0; i < 8; ++i) {
        CHECK(c.points[i][0] == -c.points[i][1]);
        mean += c.points[i][0] / 8.;
    }
    for (int i = 0; i < 8; ++i) {
        variance += (c.points[i][0] - mean) * (c.points[i][0] - mean) / 7.;
    }
    CHECK(fabs(entries[0].sample_covariance - variance) < 1e-12 &&
          fabs(entries[1].sample_covariance + variance) < 1e-12);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 1, &info, entries, 10) == RFMODEL_OK &&
          info.dimension == 2 && info.count == 8);
    CHECK(fabs(entries[1].sample_covariance + variance) < 1e-12);
    CHECK(rfmodel_monte_carlo_get_summary(r, &summary) == RFMODEL_OK &&
          summary.random_words == 32 && c.calls == 8);
    const rfmodel_monte_carlo_dependence old_dep = dependence;
    const rfmodel_monte_carlo_joint_statistics old_info = info;
    memcpy(saved, entries, sizeof(entries));
    effective[0] = 99.;
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, effective, 3) ==
              RFMODEL_INVALID_ARGUMENT &&
          memcmp(&dependence, &old_dep, sizeof(dependence)) == 0 && effective[0] == 99.);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, NULL, 4) == RFMODEL_INVALID_ARGUMENT &&
          memcmp(&dependence, &old_dep, sizeof(dependence)) == 0);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, effective, SIZE_MAX) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_dependence(
              r, (rfmodel_monte_carlo_dependence *)effective, effective, 5) ==
              RFMODEL_INVALID_ARGUMENT &&
          effective[0] == 99.);
    CHECK(rfmodel_monte_carlo_get_dependence(r, (rfmodel_monte_carlo_dependence *)r, NULL, 0) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, (double *)r, 4) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, entries, 8) ==
              RFMODEL_INVALID_ARGUMENT &&
          memcmp(&info, &old_info, sizeof(info)) == 0 &&
          memcmp(entries, saved, sizeof(entries)) == 0);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, NULL, 9) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, entries, SIZE_MAX) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(
              r, 0, (rfmodel_monte_carlo_joint_statistics *)entries, entries, 10) ==
              RFMODEL_INVALID_ARGUMENT &&
          memcmp(entries, saved, sizeof(entries)) == 0);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(
              r, 0, (rfmodel_monte_carlo_joint_statistics *)r, NULL, 0) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(
              r, 0, &info, (rfmodel_monte_carlo_joint_value *)r, 9) == RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 2, &info, NULL, 0) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, -1, &info, NULL, 0) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 1, &info, NULL, 0) == RFMODEL_OK &&
          info.count == 8 && c.calls == 8);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    // Extra inputs are validated before user callbacks or out-slot writes.
    c.calls = 0;
    c.mode = 0;
    ext.joint_statistics = 1;
    ext.correlation_count = 4;
    matrix[1] = matrix[2] = -1.;
    for (int which = 0; which < 7; ++which) {
        rfmodel_monte_carlo_extension bad = ext;
        double invalid_matrix[4] = {1., .5, .5, 1.};
        switch (which) {
        case 0:
            bad.joint_statistics = 2;
            break;
        case 1:
            bad.correlation_count = 3;
            break;
        case 2:
            bad.correlation_count = SIZE_MAX;
            break;
        case 3:
            bad.latent_correlation = NULL;
            break;
        case 4:
            invalid_matrix[1] = NAN;
            bad.latent_correlation = invalid_matrix;
            break;
        case 5:
            invalid_matrix[1] = .6;
            bad.latent_correlation = invalid_matrix;
            break;
        default:
            invalid_matrix[0] = 0.;
            bad.latent_correlation = invalid_matrix;
            break;
        }
        CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, &o, &bad, evaluate, cancel, &c, &r) ==
                  RFMODEL_INVALID_ARGUMENT &&
              !r && !c.calls);
    }
    const rfmodel_monte_carlo_extension old_ext = ext;
    CHECK(rfmodel_run_monte_carlo_extended(
              p, 2, 3, &o, &ext, evaluate, NULL, &c, (rfmodel_monte_carlo_result **)&ext) ==
              RFMODEL_INVALID_ARGUMENT &&
          memcmp(&ext, &old_ext, sizeof(ext)) == 0);
    CHECK(rfmodel_run_monte_carlo_extended(
              p, 2, 3, &o, &ext, evaluate, NULL, &c, (rfmodel_monte_carlo_result **)matrix) ==
              RFMODEL_INVALID_ARGUMENT &&
          matrix[0] == 1.);
    // Cancellation retains matrix metadata and defined zero-count dimensions.
    c.cancelled = 1;
    CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, NULL, &ext, evaluate, cancel, &c, &r) ==
          RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, NULL, 0) == RFMODEL_OK &&
          dependence.latent_rank == 1);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 1, &info, entries, 4) == RFMODEL_OK &&
          info.dimension == 2 && info.count == 0 && !entries[0].has_covariance &&
          entries[0].sample_covariance == 0.);
    CHECK(rfmodel_monte_carlo_get_summary(r, &summary) == RFMODEL_OK &&
          summary.stop == RFMODEL_MC_CANCELLED && summary.random_words == 0);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    c.cancelled = -1;
    CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, &o, &ext, evaluate, cancel, &c, &r) ==
              RFMODEL_SOLVER_ERROR &&
          !r);
    c.cancelled = 0;
    for (int mode = 2; mode <= 4; ++mode) {
        c.mode = mode;
        c.calls = 0;
        o.samples = mode == 3 ? 2 : 8;
        o.reject_invalid = 1;
        CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, &o, &ext, evaluate, NULL, &c, &r) ==
              RFMODEL_OK);
        CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, entries, 10) == RFMODEL_OK &&
              info.count == (mode == 2   ? 0
                             : mode == 3 ? 2
                                         : 4));
        if (mode == 2) {
            CHECK(!entries[0].has_covariance && !entries[0].has_correlation &&
                  entries[0].sample_covariance == 0.);
        }
        if (mode == 3) {
            CHECK(!entries[0].has_covariance && entries[0].sample_covariance == 0. &&
                  entries[1].has_covariance && entries[1].has_correlation &&
                  fabs(entries[1].sample_correlation + 1.) < 1e-14 &&
                  fabs(entries[1].sample_covariance / (-2. * (DBL_MAX * 1e-308)) - 1.) < 1e-12);
        }
        CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 1, &info, NULL, 0) == RFMODEL_OK &&
              info.count == (mode == 2   ? 0
                             : mode == 3 ? 2
                                         : 4));
        rfmodel_monte_carlo_destroy(r);
        r = NULL;
    }
    o.samples = 1;
    c.mode = 0;
    CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, &o, &ext, evaluate, NULL, &c, &r) ==
          RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, entries, 9) == RFMODEL_OK &&
          info.count == 1 && !entries[0].has_covariance);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    // NULL extension and the original function retain legacy behavior.
    CHECK(rfmodel_run_monte_carlo_extended(p, 2, 3, &o, NULL, evaluate, NULL, &c, &r) ==
          RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, NULL, 0) == RFMODEL_OK &&
          !dependence.correlation_count && !dependence.latent_rank &&
          !dependence.joint_statistics && dependence.algorithm == RFMODEL_MC_INDEPENDENT_V1);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(r, 0, &info, NULL, 0) == RFMODEL_OK &&
          !info.dimension && !info.count);
    rfmodel_monte_carlo_destroy(r);
    r = NULL;
    CHECK(rfmodel_run_monte_carlo(p, 2, 3, &o, evaluate, NULL, &c, &r) == RFMODEL_OK);
    CHECK(rfmodel_monte_carlo_get_dependence(r, &dependence, NULL, 0) == RFMODEL_OK &&
          dependence.algorithm == RFMODEL_MC_INDEPENDENT_V1);
    rfmodel_monte_carlo_destroy(r);
    CHECK(rfmodel_monte_carlo_get_dependence(NULL, &dependence, NULL, 0) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_monte_carlo_get_joint_statistics(NULL, 0, &info, NULL, 0) ==
          RFMODEL_INVALID_ARGUMENT);
    return 0;
}
