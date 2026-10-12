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

int main(void) {
    rfmodel_network *network = NULL;
    const rfmodel_complex thru[4] = {{0, 0}, {1, 0}, {1, 0}, {0, 0}};
    const rfmodel_complex refs1[2] = {{50, 0}, {25, 0}}, refs2[2] = {{100, 0}, {50, 0}};
    rfmodel_complex scattering[4], refs[5], incident[4], outgoing[4];
    const size_t external[2] = {0, 3};
    size_t offset = 99, count = 99;
    double residual = 99;
    CHECK(rfmodel_network_create(50., &network) == RFMODEL_OK);
    CHECK(rfmodel_network_get_references(network, NULL, 0) == RFMODEL_OK);
    CHECK(rfmodel_network_add_power_waves(network, 2, thru, 4, refs1, 2, &offset) == RFMODEL_OK &&
          offset == 0);
    CHECK(rfmodel_network_add_power_waves(network, 2, thru, 4, refs2, 2, &offset) == RFMODEL_OK &&
          offset == 2);
    memset(refs, 0x35, sizeof(refs));
    CHECK(rfmodel_network_get_references(network, refs, 5) == RFMODEL_OK);
    CHECK(memcmp(refs, refs1, sizeof(refs1)) == 0 && memcmp(refs + 2, refs2, sizeof(refs2)) == 0);
    {
        rfmodel_complex tail;
        memset(&tail, 0x35, sizeof(tail));
        CHECK(memcmp(refs + 4, &tail, sizeof(tail)) == 0);
    }
    CHECK(rfmodel_network_connect(network, 1, 2) == RFMODEL_OK);
    CHECK(rfmodel_network_external_s(network, external, 2, scattering, 4) == RFMODEL_OK);
    CHECK(fabs(scattering[0].real - .6) < 1e-12 && fabs(scattering[1].real - .8) < 1e-12);
    CHECK(fabs(scattering[2].real - .8) < 1e-12 && fabs(scattering[3].real + .6) < 1e-12);
    {
        const rfmodel_complex zero = {0, 0}, one = {1, 0};
        CHECK(rfmodel_network_terminate(network, 0, zero, one) == RFMODEL_OK);
        CHECK(rfmodel_network_terminate(network, 3, zero, zero) == RFMODEL_OK);
    }
    CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) == RFMODEL_OK);
    CHECK(fabs(outgoing[0].real - .6) < 1e-12 && fabs(outgoing[3].real - .8) < 1e-12 &&
          residual < 1e-12);
    rfmodel_network_destroy(network);
    CHECK(rfmodel_network_create(50., &network) == RFMODEL_OK);
    /* New add is transactional, including aliased scalar output buffers. */
    for (int scenario = 0; scenario < 17; ++scenario) {
        rfmodel_complex s[4], z[2], saved_s[4], saved_z[2];
        const rfmodel_complex *sp = s, *zp = z;
        size_t ports = 2, values = 4, references = 2, *output = &offset;
        memcpy(s, thru, sizeof(s));
        memcpy(z, refs1, sizeof(z));
        offset = 99;
        switch (scenario) {
        case 0:
            ports = 0;
            break;
        case 1:
            ports = 1025;
            break;
        case 2:
            values = 3;
            break;
        case 3:
            references = 1;
            break;
        case 4:
            references = SIZE_MAX;
            break;
        case 5:
            sp = NULL;
            break;
        case 6:
            zp = NULL;
            break;
        case 7:
            output = NULL;
            break;
        case 8:
            z[0].real = 0.;
            break;
        case 9:
            z[1].real = -1.;
            break;
        case 10:
            z[0].imag = NAN;
            break;
        case 11:
            z[1].real = INFINITY;
            break;
        case 12:
            s[0].real = NAN;
            break;
        case 13:
            output = (size_t *)s;
            break;
        case 14:
            output = (size_t *)(z + 1);
            break;
        case 15:
            output = (size_t *)network;
            break;
        case 16:
            values = SIZE_MAX;
            break;
        }
        memcpy(saved_s, s, sizeof(s));
        memcpy(saved_z, z, sizeof(z));
        CHECK(rfmodel_network_add_power_waves(network, ports, sp, values, zp, references, output) !=
              RFMODEL_OK);
        CHECK(offset == 99 && memcmp(s, saved_s, sizeof(s)) == 0 &&
              memcmp(z, saved_z, sizeof(z)) == 0);
        CHECK(rfmodel_network_port_count(network, &count) == RFMODEL_OK && count == 0);
    }
    {
        const rfmodel_complex complex_refs[2] = {{31., 12.}, {73., -21.}};
        CHECK(rfmodel_network_add_power_waves(network, 2, thru, 4, complex_refs, 2, &offset) ==
                  RFMODEL_OK &&
              offset == 0);
        CHECK(rfmodel_network_get_references(network, refs, 2) == RFMODEL_OK);
        CHECK(memcmp(refs, complex_refs, sizeof(complex_refs)) == 0);
    }
    for (int scenario = 0; scenario < 5; ++scenario) {
        rfmodel_complex result[3], saved[3], *output = result;
        size_t capacity = 3;
        memset(result, 0x26, sizeof(result));
        memcpy(saved, result, sizeof(result));
        if (scenario == 0) {
            capacity = 1;
        }
        if (scenario == 1) {
            output = NULL;
        }
        if (scenario == 2) {
            capacity = SIZE_MAX;
        }
        if (scenario == 3) {
            output = (rfmodel_complex *)network;
        }
        if (scenario == 4) {
            capacity = 0;
            output = NULL;
        }
        CHECK(rfmodel_network_get_references(network, output, capacity) != RFMODEL_OK);
        CHECK(memcmp(result, saved, sizeof(result)) == 0);
        CHECK(rfmodel_network_port_count(network, &count) == RFMODEL_OK && count == 2);
    }
    CHECK(rfmodel_network_get_references(NULL, refs, 2) != RFMODEL_OK);
    CHECK(rfmodel_network_add_power_waves(NULL, 2, thru, 4, refs1, 2, &offset) != RFMODEL_OK);
    /* Legacy blocks can coexist; their actual references remain the common value. */
    CHECK(rfmodel_network_add(network, 2, thru, 4, 50., &offset) == RFMODEL_OK && offset == 2);
    CHECK(rfmodel_network_get_references(network, refs, 4) == RFMODEL_OK && refs[2].real == 50. &&
          refs[3].imag == 0.);
    rfmodel_network_destroy(network);
    return 0;
}
