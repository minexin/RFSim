#include "rfmodel/c_api.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr,                                                                        \
                    "frequency-plan C check failed at %d: %s\n",                                   \
                    __LINE__,                                                                      \
                    rfmodel_last_error());                                                         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    size_t ports[3] = {0, 1, 2};
    rfmodel_frequency_channel seeds[2] = {{1, 3}, {0, 5}};
    rfmodel_frequency_rule rule = {RFMODEL_FREQUENCY_MIXER, 0, 1, 2, NULL, 0};
    rfmodel_frequency_device device = {ports, 3, seeds, 2, &rule, 1};
    rfmodel_frequency_plan_request request = {1e6, &device, 1, NULL, 0, NULL};
    rfmodel_frequency_plan_diagnostics diagnostics, saved_diagnostics;
    rfmodel_frequency_channel channels[8], saved_channels[8];
    size_t offsets[3], mappings[3], saved_offsets[3], saved_mappings[3];
    rfmodel_frequency_plan_output output = {channels, 8, offsets, 3, mappings, 3};
    int scenario;
    CHECK(rfmodel_plan_conversion_frequencies(&request, NULL, &diagnostics) == RFMODEL_OK);
    CHECK(diagnostics.channel_count == 4 && diagnostics.seed_count == 2 &&
          diagnostics.generated_channels == 2 && diagnostics.device_count == 1);
    memset(channels, 0x5a, sizeof(channels));
    memset(offsets, 0x5a, sizeof(offsets));
    memset(mappings, 0x5a, sizeof(mappings));
    memcpy(saved_channels, channels, sizeof(channels));
    memcpy(saved_offsets, offsets, sizeof(offsets));
    memcpy(saved_mappings, mappings, sizeof(mappings));
    CHECK(rfmodel_plan_conversion_frequencies(&request, &output, &diagnostics) == RFMODEL_OK);
    CHECK(channels[0].port == 0 && channels[0].bin == 5 && channels[1].port == 1 &&
          channels[1].bin == 3 && channels[2].port == 2 && channels[2].bin == 2 &&
          channels[3].port == 2 && channels[3].bin == 8);
    CHECK(offsets[0] == 0 && offsets[1] == 4 && mappings[0] == 1 && mappings[1] == 0);
    CHECK(memcmp(channels + 4, saved_channels + 4, 4 * sizeof(channels[0])) == 0);
    CHECK(offsets[2] == saved_offsets[2] && mappings[2] == saved_mappings[2]);
    for (scenario = 0; scenario < 13; ++scenario) {
        rfmodel_frequency_plan_request r = request;
        rfmodel_frequency_plan_output o = output;
        rfmodel_frequency_plan_diagnostics *diag = &diagnostics;
        rfmodel_frequency_plan_options bounds = {512, INT_MAX, 10000000};
        rfmodel_frequency_device changed = device;
        rfmodel_frequency_rule changed_rule = rule;
        memset(channels, 0x4b, sizeof(channels));
        memset(offsets, 0x4b, sizeof(offsets));
        memset(mappings, 0x4b, sizeof(mappings));
        memset(&diagnostics, 0x4b, sizeof(diagnostics));
        memcpy(saved_channels, channels, sizeof(channels));
        memcpy(saved_offsets, offsets, sizeof(offsets));
        memcpy(saved_mappings, mappings, sizeof(mappings));
        saved_diagnostics = diagnostics;
        if (scenario == 0) {
            o.channel_capacity = 3;
        }
        if (scenario == 1) {
            o.device_offset_capacity = 1;
        }
        if (scenario == 2) {
            o.seed_capacity = 1;
        }
        if (scenario == 3) {
            o.channel_capacity = (size_t)-1;
        }
        if (scenario == 4) {
            o.seed_channel_indices = o.device_offsets;
        }
        if (scenario == 5) {
            bounds.max_bin = 7;
            r.options = &bounds;
        }
        if (scenario == 6) {
            bounds.max_transitions = 1;
            r.options = &bounds;
        }
        if (scenario == 7) {
            changed.seed_count = (size_t)-1;
            r.devices = &changed;
        }
        if (scenario == 8) {
            changed_rule.kind = (rfmodel_frequency_rule_kind)99;
            changed.rules = &changed_rule;
            r.devices = &changed;
        }
        if (scenario == 9) {
            changed_rule.coefficient_count = 1;
            changed.rules = &changed_rule;
            r.devices = &changed;
        }
        if (scenario == 10) {
            o.channels = NULL;
        }
        if (scenario == 11) {
            diag = NULL;
        }
        if (scenario == 12) {
            diag = (rfmodel_frequency_plan_diagnostics *)(channels + 4);
        }
        CHECK(rfmodel_plan_conversion_frequencies(&r, &o, diag) != RFMODEL_OK);
        CHECK(memcmp(channels, saved_channels, sizeof(channels)) == 0);
        CHECK(memcmp(offsets, saved_offsets, sizeof(offsets)) == 0);
        CHECK(memcmp(mappings, saved_mappings, sizeof(mappings)) == 0);
        CHECK(memcmp(&diagnostics, &saved_diagnostics, sizeof(diagnostics)) == 0);
    }
    /* All nested inputs and descriptor storage remain protected, including
       unused output capacity. Inputs are valid before the aliasing attempt. */
    {
        union {
            size_t ports[32];
            rfmodel_frequency_channel output[16];
        } storage;

        unsigned char saved[sizeof(storage)];
        rfmodel_frequency_device alias_device = device;
        rfmodel_frequency_plan_request r = request;
        rfmodel_frequency_plan_output o = output;
        memset(&storage, 0, sizeof(storage));
        storage.ports[0] = 0;
        storage.ports[1] = 1;
        storage.ports[2] = 2;
        memcpy(saved, &storage, sizeof(storage));
        alias_device.ports = storage.ports;
        r.devices = &alias_device;
        o.channels = storage.output;
        CHECK(rfmodel_plan_conversion_frequencies(&r, &o, &diagnostics) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
    }
    {
        double coefficients[8] = {.2, 0., 1., 0., 0., 0., 0., 0.};
        double saved[8];
        rfmodel_frequency_channel seed = {0, 1};
        rfmodel_frequency_rule polynomial = {
            RFMODEL_FREQUENCY_POLYNOMIAL, 0, 0, 1, coefficients, 3};
        rfmodel_frequency_device d = {ports, 2, &seed, 1, &polynomial, 1};
        rfmodel_frequency_plan_request r = {1., &d, 1, NULL, 0, NULL};
        rfmodel_frequency_plan_output o = output;
        CHECK(rfmodel_plan_conversion_frequencies(&r, &o, &diagnostics) == RFMODEL_OK);
        CHECK(diagnostics.channel_count == 3 && channels[1].bin == 0 && channels[2].bin == 2);
        memcpy(saved, coefficients, sizeof(saved));
        o.seed_channel_indices = (size_t *)coefficients;
        CHECK(rfmodel_plan_conversion_frequencies(&r, &o, &diagnostics) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, coefficients, sizeof(saved)) == 0);
        {
            rfmodel_frequency_rule saved_rule = polynomial;
            CHECK(rfmodel_plan_conversion_frequencies(
                      &r, NULL, (rfmodel_frequency_plan_diagnostics *)&polynomial) ==
                  RFMODEL_INVALID_ARGUMENT);
            CHECK(memcmp(&polynomial, &saved_rule, sizeof(polynomial)) == 0);
        }
    }
    {
        union {
            rfmodel_frequency_plan_output descriptor;
            unsigned char bytes[256];
        } storage;

        unsigned char saved[sizeof(storage)];
        memset(&storage, 0, sizeof(storage));
        storage.descriptor = output;
        storage.descriptor.channels = (rfmodel_frequency_channel *)&storage;
        memcpy(saved, &storage, sizeof(storage));
        CHECK(rfmodel_plan_conversion_frequencies(&request, &storage.descriptor, &diagnostics) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(memcmp(saved, &storage, sizeof(storage)) == 0);
    }
    /* A constant polynomial can close through a physical loop without any seeds. */
    {
        const double coefficients[1] = {1.};
        const rfmodel_frequency_rule polynomial = {
            RFMODEL_FREQUENCY_POLYNOMIAL, 0, 0, 1, coefficients, 1};
        const rfmodel_frequency_device d = {ports, 2, NULL, 0, &polynomial, 1};
        const rfmodel_conversion_connection connection = {0, 0, 0, 1};
        const rfmodel_frequency_plan_request r = {1., &d, 1, &connection, 1, NULL};
        rfmodel_frequency_plan_output o = {channels, 6, offsets, 3, NULL, 0};
        CHECK(rfmodel_plan_conversion_frequencies(&r, &o, &diagnostics) == RFMODEL_OK);
        CHECK(diagnostics.seed_count == 0 && diagnostics.channel_count == 2 &&
              diagnostics.generated_channels == 2 && channels[0].bin == 0 && channels[1].bin == 0);
    }
    puts("frequency-plan C ABI tests passed");
    return 0;
}
