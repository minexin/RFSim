#include "rfmodel/api/c_api.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "highorder-plan C check %d: %s\n", __LINE__, rfmodel_last_error());    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    const size_t ports[2] = {0, 1}, source_port = 7;
    const rfmodel_frequency_channel seeds[2] = {{7, 13}, {7, 1}};
    rfmodel_frequency_device devices[2] = {{ports, 2, NULL, 0, NULL, 0},
                                           {&source_port, 1, seeds, 2, NULL, 0}};
    const rfmodel_conversion_connection connection = {0, 0, 1, 7};
    rfmodel_frequency_plan_request request = {1., devices, 2, &connection, 1, NULL};
    double coefficients[4] = {0., 0., .1, .2};
    int generating[2] = {1, 1};
    rfmodel_highorder_frequency_rule rule = {0, 0, 1, coefficients, 4, generating, 1};
    rfmodel_frequency_plan_diagnostics diagnostics, saved_diagnostics;
    rfmodel_frequency_channel channels[12], saved_channels[12];
    size_t offsets[4], mappings[4], saved_offsets[4], saved_mappings[4];
    rfmodel_frequency_plan_output output = {channels, 12, offsets, 4, mappings, 4};
    int scenario;
    CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
              &request, &rule, 1, NULL, &diagnostics) == RFMODEL_OK);
    CHECK(diagnostics.channel_count == 8 && diagnostics.seed_count == 2 &&
          diagnostics.generated_channels == 6);
    memset(channels, 0x5a, sizeof(channels));
    memset(offsets, 0x5a, sizeof(offsets));
    memset(mappings, 0x5a, sizeof(mappings));
    memcpy(saved_channels, channels, sizeof(channels));
    memcpy(saved_offsets, offsets, sizeof(offsets));
    memcpy(saved_mappings, mappings, sizeof(mappings));
    CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
              &request, &rule, 1, &output, &diagnostics) == RFMODEL_OK);
    CHECK(offsets[0] == 0 && offsets[1] == 6 && offsets[2] == 8 && mappings[0] == 1 &&
          mappings[1] == 0);
    CHECK(channels[0].port == 0 && channels[0].bin == 1 && channels[1].bin == 13 &&
          channels[2].port == 1 && channels[2].bin == 1 && channels[3].bin == 2 &&
          channels[4].bin == 3 && channels[5].bin == 13);
    CHECK(memcmp(channels + 8, saved_channels + 8, 4 * sizeof(channels[0])) == 0);
    CHECK(offsets[3] == saved_offsets[3] && mappings[2] == saved_mappings[2]);
    for (scenario = 0; scenario < 24; ++scenario) {
        rfmodel_highorder_frequency_rule changed = rule;
        rfmodel_frequency_plan_request r = request;
        rfmodel_frequency_plan_output o = output;
        rfmodel_frequency_device changed_devices[2] = {devices[0], devices[1]};
        rfmodel_frequency_plan_options bounds = {512, INT_MAX, 10000000};
        rfmodel_frequency_rule copy = {RFMODEL_FREQUENCY_COPY, 0, 0, 1, NULL, 0};
        double bad_coefficients[4] = {0., 0., .1, .2};
        int bad_bin = 0;
        size_t count = 1;
        const rfmodel_highorder_frequency_rule *rules = &changed;
        memset(channels, 0x4b, sizeof(channels));
        memset(offsets, 0x4b, sizeof(offsets));
        memset(mappings, 0x4b, sizeof(mappings));
        memset(&diagnostics, 0x4b, sizeof(diagnostics));
        memcpy(saved_channels, channels, sizeof(channels));
        memcpy(saved_offsets, offsets, sizeof(offsets));
        memcpy(saved_mappings, mappings, sizeof(mappings));
        saved_diagnostics = diagnostics;
        switch (scenario) {
        case 0:
            changed.device = 2;
            break;
        case 1:
            changed.voltage_coefficients = NULL;
            break;
        case 2:
            changed.coefficient_count = 0;
            break;
        case 3:
            changed.coefficient_count = 13;
            break;
        case 4:
            changed.generating_bins = NULL;
            break;
        case 5:
            changed.generating_count = 513;
            break;
        case 6:
            changed.generating_bins = &bad_bin;
            break;
        case 7:
            bad_bin = -1;
            changed.generating_bins = &bad_bin;
            break;
        case 8:
            changed.generating_count = 2;
            break;
        case 9:
            bad_bin = 2;
            changed.generating_bins = &bad_bin;
            break;
        case 10:
            bad_coefficients[0] = 1.;
            changed.voltage_coefficients = bad_coefficients;
            break;
        case 11:
            bad_coefficients[1] = 1.;
            changed.voltage_coefficients = bad_coefficients;
            break;
        case 12:
            bad_coefficients[2] = NAN;
            changed.voltage_coefficients = bad_coefficients;
            break;
        case 13:
            changed.output_port = 0;
            break;
        case 14:
            changed.input_port = 99;
            break;
        case 15:
            o.channel_capacity = 7;
            break;
        case 16:
            bounds.max_bin = 2;
            r.options = &bounds;
            break;
        case 17:
            bounds.max_total_channels = 7;
            r.options = &bounds;
            break;
        case 18:
            bounds.max_transitions = 1;
            r.options = &bounds;
            break;
        case 19:
            count = (size_t)-1;
            break;
        case 20:
            rules = NULL;
            break;
        case 21:
            count = 4097;
            break;
        case 22:
            changed_devices[0].rules = &copy;
            changed_devices[0].rule_count = 4096;
            r.devices = changed_devices;
            break;
        case 23:
            o.seed_capacity = 1;
            break;
        }
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &r, rules, count, &o, &diagnostics) != RFMODEL_OK);
        CHECK(memcmp(channels, saved_channels, sizeof(channels)) == 0);
        CHECK(memcmp(offsets, saved_offsets, sizeof(offsets)) == 0);
        CHECK(memcmp(mappings, saved_mappings, sizeof(mappings)) == 0);
        CHECK(memcmp(&diagnostics, &saved_diagnostics, sizeof(diagnostics)) == 0);
    }
    /* Supplemental descriptors and both nested arrays are protected, including
       unused capacity tails and diagnostics in size-query calls. */
    {
        union {
            double coefficients[32];
            rfmodel_frequency_channel channels[16];
            rfmodel_frequency_plan_diagnostics diag;
        } storage;

        unsigned char saved[sizeof(storage)];
        rfmodel_highorder_frequency_rule changed = rule;
        rfmodel_frequency_plan_output o = output;
        memset(&storage, 0, sizeof(storage));
        storage.coefficients[2] = .1;
        changed.voltage_coefficients = storage.coefficients;
        changed.coefficient_count = 4;
        memcpy(saved, &storage, sizeof(storage));
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &changed, 1, NULL, &diagnostics) == RFMODEL_OK);
        o.channels = storage.channels;
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &changed, 1, &o, &diagnostics) == RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &changed, 1, NULL, &storage.diag) == RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
    }
    {
        union {
            int bins[64];
            rfmodel_frequency_channel channels[16];
            rfmodel_frequency_plan_diagnostics diag;
        } storage;

        unsigned char saved[sizeof(storage)];
        rfmodel_highorder_frequency_rule changed = rule;
        rfmodel_frequency_plan_output o = output;
        memset(&storage, 0, sizeof(storage));
        storage.bins[40] = 1;
        changed.generating_bins = storage.bins + 40;
        memcpy(saved, &storage, sizeof(storage));
        o.channels = storage.channels;
        o.channel_capacity = 16;
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &changed, 1, &o, &diagnostics) == RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
        changed.generating_bins = storage.bins;
        storage.bins[0] = 1;
        memcpy(saved, &storage, sizeof(storage));
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &changed, 1, NULL, &storage.diag) == RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
    }
    {
        union {
            rfmodel_highorder_frequency_rule rule;
            rfmodel_frequency_channel channels[16];
            rfmodel_frequency_plan_diagnostics diag;
        } storage;

        unsigned char saved[sizeof(storage)];
        rfmodel_frequency_plan_output o = output;
        memset(&storage, 0, sizeof(storage));
        storage.rule = rule;
        memcpy(saved, &storage, sizeof(storage));
        o.channels = storage.channels;
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &storage.rule, 1, &o, &diagnostics) == RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, &storage.rule, 1, NULL, &storage.diag) == RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
    }
    /* Empty generation keeps direct RF only, and no supplemental rules means
       exact equivalence to the original exported function. */
    rule.generating_bins = NULL;
    rule.generating_count = 0;
    CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
              &request, &rule, 1, &output, &diagnostics) == RFMODEL_OK);
    CHECK(diagnostics.channel_count == 6 && channels[2].bin == 1 && channels[3].bin == 13);
    {
        const rfmodel_frequency_rule copy = {RFMODEL_FREQUENCY_COPY, 0, 0, 1, NULL, 0};
        devices[0].rules = &copy;
        devices[0].rule_count = 1;
        CHECK(rfmodel_plan_conversion_frequencies(&request, &output, &saved_diagnostics) ==
              RFMODEL_OK);
        CHECK(rfmodel_plan_conversion_frequencies_with_highorders(
                  &request, NULL, 0, &output, &diagnostics) == RFMODEL_OK);
        CHECK(memcmp(&diagnostics, &saved_diagnostics, sizeof(diagnostics)) == 0);
    }
    puts("high-order frequency-plan C ABI tests passed");
    return 0;
}
