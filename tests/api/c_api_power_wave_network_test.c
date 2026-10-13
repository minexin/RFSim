#include "rfmodel/api/c_api.h"
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

int main(void) {
    rfmodel_network *network = NULL, *reduced = NULL;
    const rfmodel_complex s[4] = {{5. / 7., 0}, {4. / 7., 0}, {4. / 7., 0}, {-1. / 7., 0}};
    const rfmodel_complex refs[2] = {{25, 0}, {100, 0}};
    size_t ports[2] = {0, 1}, offset = 99, count = 99;
    rfmodel_complex actual[4], original[4], local[2];
    CHECK(rfmodel_network_create(75., &network) == RFMODEL_OK);
    CHECK(rfmodel_network_add_power_waves(network, 2, s, 4, refs, 2, &offset) == RFMODEL_OK);
    CHECK(rfmodel_network_external_s(network, ports, 2, original, 4) == RFMODEL_OK);
    for (int bad = 0; bad < 12; ++bad) {
        size_t selected[2] = {0, 1}, n = 2;
        double reference = 50.;
        const rfmodel_network *input = network;
        const size_t *selection = selected;
        rfmodel_network *output = NULL;
        rfmodel_network **destination = &output;
        if (bad == 0) {
            input = NULL;
        }
        if (bad == 1) {
            selection = NULL;
        }
        if (bad == 2) {
            n = 0;
        }
        if (bad == 3) {
            n = 1025;
        }
        if (bad == 4) {
            selected[1] = 0;
        }
        if (bad == 5) {
            selected[1] = 7;
        }
        if (bad == 6) {
            reference = 0.;
        }
        if (bad == 7) {
            reference = NAN;
        }
        if (bad == 8) {
            reference = INFINITY;
        }
        if (bad == 9) {
            destination = NULL;
        }
        if (bad == 10) {
            output = network; /* Never overwrite an owned non-null handle. */
        }
        if (bad == 11) {
            destination = (rfmodel_network **)network;
        }
        CHECK(rfmodel_network_reduce_power_waves(input, selection, n, reference, destination) !=
              RFMODEL_OK);
        CHECK(output == (bad == 10 ? network : NULL));
        CHECK(rfmodel_network_external_s(network, ports, 2, actual, 4) == RFMODEL_OK);
        CHECK(memcmp(actual, original, sizeof(actual)) == 0);
    }
    {
        union {
            size_t ports[2];
            rfmodel_network *pointer;
        } alias;

        alias.ports[0] = 0;
        alias.ports[1] = 1;
        size_t saved[2];
        memcpy(saved, alias.ports, sizeof(saved));
        CHECK(rfmodel_network_reduce_power_waves(network, alias.ports, 2, 50., &alias.pointer) !=
              RFMODEL_OK);
        CHECK(memcmp(saved, alias.ports, sizeof(saved)) == 0);
    }
    CHECK(rfmodel_network_reduce_power_waves(network, ports, 2, 50., &reduced) == RFMODEL_OK);
    CHECK(rfmodel_network_port_count(reduced, &count) == RFMODEL_OK && count == 2);
    CHECK(rfmodel_network_get_references(reduced, local, 2) == RFMODEL_OK);
    CHECK(local[0].real == 50. && local[1].real == 50. && local[0].imag == 0.);
    CHECK(rfmodel_network_external_s(reduced, ports, 2, actual, 4) == RFMODEL_OK);
    CHECK(fabs(actual[0].real - 1. / 3.) < 1e-12 && fabs(actual[2].real - 2. / 3.) < 1e-12);
    rfmodel_network_destroy(network);
    network = NULL;
    {
        const rfmodel_spectrum_bin input[2] = {{0, {.3, 0}}, {1, {.3, .15}}};
        rfmodel_spectrum_bin output[2];
        CHECK(rfmodel_network_transmit_spectrum(
                  reduced, ports, 2, 1e9, input, 2, output, 2, &count) == RFMODEL_OK);
        CHECK(count == 2 && fabs(output[0].amplitude.real - .2) < 1e-12);
        CHECK(fabs(output[1].amplitude.imag - .1) < 1e-12);
    }
    /* A returned snapshot is an ordinary network with independent lifetime. */
    CHECK(rfmodel_network_reduce_power_waves(reduced, ports, 2, 75., &network) == RFMODEL_OK);
    rfmodel_network_destroy(reduced);
    reduced = NULL;
    CHECK(rfmodel_network_external_s(network, ports, 2, actual, 4) == RFMODEL_OK);
    CHECK(fabs(actual[2].real - .75) < 1e-12);
    rfmodel_network_destroy(network);
    return 0;
}
