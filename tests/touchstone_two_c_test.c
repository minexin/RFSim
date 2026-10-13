#include "rfmodel/c_api.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* These checks deliberately do not use assert: Release must exercise the ABI. */
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr,                                                                        \
                    "Touchstone 2 C ABI line %d: %s: %s\n",                                        \
                    __LINE__,                                                                      \
                    #expression,                                                                   \
                    rfmodel_last_error());                                                         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static size_t numerical_checks = 0;
static double maximum_scaled_error = 0.;

static int close_value(rfmodel_complex actual, double expected, double scale) {
    const double error = hypot(actual.real - expected, actual.imag) / scale;
    if (!isfinite(error) || error > 3e-12) {
        fprintf(stderr,
                "actual %.17g%+.17gj expected %.17g; scaled error %.17g\n",
                actual.real,
                actual.imag,
                expected,
                error);
        return 0;
    }
    if (error > maximum_scaled_error) {
        maximum_scaled_error = error;
    }
    ++numerical_checks;
    return 1;
}

static int write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    int ok;
    if (!file) {
        return 0;
    }
    ok = fputs(content, file) >= 0;
    return fclose(file) == 0 && ok;
}

static int test_common_reference_snapshot(const char *path) {
    /* Direct scalar wave equations for a matched 75-ohm gain-2 amplifier
       expressed at 50 ohms give S=[.2,0;1.92,.2]. The noise parameters retain
       their option-line 75-ohm reference, and physical Rn is 37.5 ohms. */
    static const char fixture[] =
        "[Version] 2.0\n# GHz S RI R 75\n[Number of Ports] 2\n"
        "[Two-Port Data Order] 12_21\n[Number of Frequencies] 2\n"
        "[Reference] 50 50\n[Number of Noise Frequencies] 2\n[Network Data]\n"
        "1 .2 0 0 0 1.92 0 .2 0\n5 .2 0 0 0 1.92 0 .2 0\n"
        "[Noise Data]\n2 3.010299956639812 0 0 37.5\n"
        "4 3.010299956639812 0 0 37.5\n[End]\n";
    const double kt = 1.380649e-23 * 290.;
    const double s50[4] = {.2, 0., 1.92, .2};
    const double s75[4] = {0., 0., 2., 0.};
    const double c50[4] = {.96, -.384, -.384, 3.9936};
    const double c75[4] = {1., 0., 0., 4.};
    const double frequencies[3] = {2e9, 3e9, 4e9};
    rfmodel_touchstone *model = NULL, *clamp = NULL;
    rfmodel_touchstone_info info;
    rfmodel_complex values[6], saved[6];
    size_t point, i;
    int scenario;
    CHECK(write_file(path, fixture));
    CHECK(rfmodel_touchstone_open(path, 0, &model) == RFMODEL_OK && model != NULL);
    CHECK(rfmodel_touchstone_open(path, 1, &clamp) == RFMODEL_OK && clamp != NULL);
    CHECK(rfmodel_touchstone_get_info(model, &info) == RFMODEL_OK);
    CHECK(info.ports == 2 && info.reference_ohms == 50. && info.noise_sample_count == 2);
    CHECK(info.minimum_frequency_hz == 1e9 && info.maximum_frequency_hz == 5e9);
    CHECK(write_file(path, "file replaced after both opens\n"));
    for (point = 0; point < 3; ++point) {
        memset(values, 0x37, sizeof(values));
        memcpy(saved, values, sizeof(values));
        CHECK(rfmodel_touchstone_s(model, frequencies[point], 50., values, 6) == RFMODEL_OK);
        for (i = 0; i < 4; ++i) {
            CHECK(close_value(values[i], s50[i], 2.));
        }
        CHECK(memcmp(values + 4, saved + 4, 2 * sizeof(*values)) == 0);
        CHECK(rfmodel_touchstone_s(model, frequencies[point], 75., values, 6) == RFMODEL_OK);
        for (i = 0; i < 4; ++i) {
            CHECK(close_value(values[i], s75[i], 2.));
        }
        CHECK(memcmp(values + 4, saved + 4, 2 * sizeof(*values)) == 0);
        CHECK(rfmodel_touchstone_noise(model, frequencies[point], 50., 290., values, 6) ==
              RFMODEL_OK);
        for (i = 0; i < 4; ++i) {
            CHECK(close_value(values[i], c50[i] * kt, kt));
        }
        CHECK(memcmp(values + 4, saved + 4, 2 * sizeof(*values)) == 0);
        CHECK(rfmodel_touchstone_noise(model, frequencies[point], 75., 290., values, 6) ==
              RFMODEL_OK);
        for (i = 0; i < 4; ++i) {
            CHECK(close_value(values[i], c75[i] * kt, kt));
        }
        CHECK(memcmp(values + 4, saved + 4, 2 * sizeof(*values)) == 0);
    }
    /* Temperature scaling and independent noise/S out-of-band policies. */
    CHECK(rfmodel_touchstone_noise(model, 3e9, 50., 580., values, 6) == RFMODEL_OK);
    for (i = 0; i < 4; ++i) {
        CHECK(close_value(values[i], 2. * c50[i] * kt, kt));
    }
    CHECK(rfmodel_touchstone_s(model, 1e9, 50., values, 6) == RFMODEL_OK);
    CHECK(rfmodel_touchstone_s(model, 5e9, 50., values, 6) == RFMODEL_OK);
    CHECK(rfmodel_touchstone_noise(clamp, 0., 50., 290., values, 6) == RFMODEL_OK);
    for (i = 0; i < 4; ++i) {
        CHECK(close_value(values[i], c50[i] * kt, kt));
    }
    CHECK(rfmodel_touchstone_noise(clamp, 9e9, 50., 290., values, 6) == RFMODEL_OK);
    for (i = 0; i < 4; ++i) {
        CHECK(close_value(values[i], c50[i] * kt, kt));
    }
    CHECK(rfmodel_touchstone_s(clamp, 9e9, 50., values, 6) == RFMODEL_OK);
    for (i = 0; i < 4; ++i) {
        CHECK(close_value(values[i], s50[i], 2.));
    }
    for (scenario = 0; scenario < 10; ++scenario) {
        double frequency = 3e9, reference = 50., temperature = 290.;
        size_t capacity = 6;
        rfmodel_complex *output = values;
        int noise_status, s_status;
        memset(values, 0x63, sizeof(values));
        memcpy(saved, values, sizeof(values));
        switch (scenario) {
        case 0:
            capacity = 3;
            break;
        case 1:
            capacity = 0;
            break;
        case 2:
            output = NULL;
            break;
        case 3:
            reference = 0.;
            break;
        case 4:
            reference = INFINITY;
            break;
        case 5:
            frequency = NAN;
            break;
        case 6:
            frequency = -1.;
            break;
        case 7:
            frequency = 0.;
            break;
        case 8:
            frequency = 9e9;
            break;
        case 9:
            temperature = 0.;
            break;
        }
        noise_status =
            rfmodel_touchstone_noise(model, frequency, reference, temperature, output, capacity);
        CHECK(noise_status != RFMODEL_OK);
        CHECK(memcmp(values, saved, sizeof(values)) == 0);
        if (scenario != 9) {
            s_status = rfmodel_touchstone_s(model, frequency, reference, output, capacity);
            CHECK(s_status != RFMODEL_OK);
            CHECK(memcmp(values, saved, sizeof(values)) == 0);
        }
    }
    /* Noise support is 2..4 GHz even though S is available on 1..5 GHz. */
    memset(values, 0x63, sizeof(values));
    memcpy(saved, values, sizeof(values));
    CHECK(rfmodel_touchstone_noise(model, 1e9, 50., 290., values, 6) != RFMODEL_OK);
    CHECK(memcmp(values, saved, sizeof(values)) == 0);
    CHECK(rfmodel_touchstone_noise(model, 5e9, 50., 290., values, 6) != RFMODEL_OK);
    CHECK(memcmp(values, saved, sizeof(values)) == 0);
    CHECK(rfmodel_touchstone_s(model, 3e9, 50., values, 6) == RFMODEL_OK);
    for (i = 0; i < 4; ++i) {
        CHECK(close_value(values[i], s50[i], 2.));
    }
    rfmodel_touchstone_close(model);
    rfmodel_touchstone_close(clamp);
    return 0;
}

static int test_unequal_open_and_malformed(const char *path) {
    static const char unequal[] = "[Version] 2.0\n# GHz S RI R 75\n[Number of Ports] 2\n"
                                  "[Two-Port Data Order] 21_12\n[Number of Frequencies] 1\n"
                                  "[Reference] 31 73\n[Network Data]\n1 0 0 1 0 1 0 0 0\n[End]\n";
    static const char bad_count[] = "[Version] 2.0\n# GHz S RI R 75\n[Number of Ports] 2\n"
                                    "[Two-Port Data Order] 21_12\n[Number of Frequencies] 2\n"
                                    "[Network Data]\n1 0 0 1 0 1 0 0 0\n[End]\n";
    rfmodel_touchstone *model = (rfmodel_touchstone *)(uintptr_t)1;
    rfmodel_touchstone_info info, saved_info;
    CHECK(write_file(path, unequal));
    CHECK(rfmodel_touchstone_open(path, 0, &model) == RFMODEL_INVALID_ARGUMENT);
    CHECK(model == NULL);
    CHECK(strstr(rfmodel_last_error(), "reference") != NULL);
    model = (rfmodel_touchstone *)(uintptr_t)1;
    CHECK(rfmodel_touchstone_open(path, 1, &model) == RFMODEL_INVALID_ARGUMENT && model == NULL);
    CHECK(write_file(path, bad_count));
    model = (rfmodel_touchstone *)(uintptr_t)1;
    CHECK(rfmodel_touchstone_open(path, 0, &model) != RFMODEL_OK && model == NULL);
    model = (rfmodel_touchstone *)(uintptr_t)1;
    CHECK(rfmodel_touchstone_open(path, 2, &model) == RFMODEL_INVALID_ARGUMENT && model == NULL);
    memset(&info, 0x32, sizeof(info));
    memcpy(&saved_info, &info, sizeof(info));
    CHECK(rfmodel_touchstone_get_info(NULL, &info) != RFMODEL_OK);
    CHECK(memcmp(&info, &saved_info, sizeof(info)) == 0);
    rfmodel_touchstone_close(NULL);
    return 0;
}

int main(void) {
    const char *path = "touchstone-two-c-api.ts";
    const int result =
        test_common_reference_snapshot(path) || test_unequal_open_and_malformed(path);
    remove(path);
    if (result) {
        return 1;
    }
    printf("{\"passed\":true,\"numerical_checks\":%lu,\"maximum_scaled_error\":%.17g}\n",
           (unsigned long)numerical_checks,
           maximum_scaled_error);
    return 0;
}
