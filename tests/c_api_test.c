#include "rfmodel/c_api.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(                                                                               \
                stderr, "C API check failed at line %d: %s\n", __LINE__, rfmodel_last_error());    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    rfmodel_network *network = NULL;
    const rfmodel_complex pad[4] = {{0, 0}, {0.5, 0}, {0.5, 0}, {0, 0}};
    const rfmodel_complex zero = {0, 0}, one = {1, 0};
    rfmodel_complex incident[4], outgoing[4], scattering[4];
    size_t offset = 999, count = 0;
    const size_t external[2] = {0, 3};
    double residual = -1;
    CHECK(rfmodel_abi_version() == 1);
    {
        const rfmodel_complex s = {0.2, 0.3}, c = {2., 0.}, e = {3., 0.}, gamma = {-0.1, 0.2};
        rfmodel_complex a = {123., 0.}, b = {456., 0.};
        double net = 789., temperature = 290.;
        CHECK(rfmodel_loaded_noise(1, &s, &c, &e, 1, &gamma, 1, &a, &b, 1, &net, 0) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(a.real == 123. && b.real == 456. && net == 789.);
        CHECK(rfmodel_loaded_noise(1, &s, &c, &e, 1, &gamma, 1, &a, &b, 1, &net, 1) == RFMODEL_OK);
        CHECK(fabs(b.real - 2.39 / 1.1665) < 1e-12);
        CHECK(fabs(a.real - 3.1 / 1.1665) < 1e-12);
        CHECK(fabs(net - (a.real - b.real)) < 1e-12);
        CHECK(rfmodel_thermal_boundary_noise(1, &gamma, &temperature, &a, 1) == RFMODEL_OK);
        CHECK(fabs(a.real / (1.380649e-23 * 290.) - 0.95) < 1e-12);
    }
    {
        rfmodel_spectrum_bin input[2] = {{10, {1., 0.}}, {10, {1., 0.}}};
        rfmodel_spectrum_bin result[16] = {{999, {123., 0.}}};
        size_t produced = 999;
        CHECK(rfmodel_ideal_mixer_transmit(1e6, input, 1, 2, 0., 0., 50., result, 1, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(produced == 999 && result[0].index == 999 && result[0].amplitude.real == 123.);
        CHECK(rfmodel_ideal_mixer_transmit(1e6, input, 2, 2, 0., 0., 50., result, 16, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_ideal_mixer_transmit(1e6, input, 1, 2, 0., 0., 50., result, 16, &produced) ==
              RFMODEL_OK);
        CHECK(produced == 2 && result[0].index == 8 && result[1].index == 12);
        CHECK(fabs(result[0].amplitude.real - 1.) < 1e-12);
        input[0].amplitude.real = sqrt(1e-5);
        input[1].index = 13;
        input[1].amplitude.real = sqrt(1e-5);
        CHECK(rfmodel_cubic_amplifier_transmit(
                  1e6, input, 2, 20., 10., 50., result, 16, &produced) == RFMODEL_OK);
        CHECK(result[0].index == 7);
        CHECK(fabs(result[0].amplitude.real * result[0].amplitude.real / 1e-9 - 1.) < 1e-12);
        CHECK(rfmodel_cubic_amplifier_transmit(1e6, NULL, 0, 20., 10., 50., NULL, 0, &produced) ==
              RFMODEL_OK);
        CHECK(produced == 0);
    }
    {
        const rfmodel_complex input_z = {50., 50.}, output_z = {50., -50.};
        const rfmodel_complex singular_z = {-50., 0.};
        CHECK(rfmodel_linear_amplifier_s(
                  1e9, 20., 90., 20., -90., input_z, output_z, 50., scattering, 4) == RFMODEL_OK);
        CHECK(fabs(scattering[0].real - 0.2) < 1e-12);
        CHECK(fabs(scattering[0].imag - 0.4) < 1e-12);
        CHECK(fabs(scattering[2].imag - 10.) < 1e-12);
        CHECK(fabs(scattering[1].imag + 0.1) < 1e-12);
        CHECK(rfmodel_linear_amplifier_s(
                  1e9, 20., 0., 20., 0., singular_z, output_z, 50., scattering, 4) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(fabs(scattering[2].imag - 10.) < 1e-12);
    }
    scattering[0].real = 123.;
    CHECK(rfmodel_transmission_line_s(1e9, 100., 0.25e-9, 0., 50., scattering, 3) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(scattering[0].real == 123.);
    CHECK(rfmodel_transmission_line_s(1e9, 100., 0.25e-9, 0., 50., scattering, 4) == RFMODEL_OK);
    CHECK(fabs(scattering[0].real - 0.6) < 1e-12 && fabs(scattering[2].imag + 0.8) < 1e-12);
    CHECK(rfmodel_rlgc_line_s(0., 25., 0., 0., 0., 2., 50., scattering, 4) == RFMODEL_OK);
    CHECK(fabs(scattering[2].real - 2. / 3.) < 1e-12);
    CHECK(rfmodel_network_create(-1, &network) == RFMODEL_INVALID_ARGUMENT);
    CHECK(network == NULL && strlen(rfmodel_last_error()) > 0);
    CHECK(rfmodel_network_create(50, &network) == RFMODEL_OK);
    CHECK(rfmodel_network_add(network, 2, pad, 3, 50, &offset) == RFMODEL_INVALID_ARGUMENT);
    CHECK(offset == 999);
    CHECK(rfmodel_network_add(network, 2, pad, 4, 50, &offset) == RFMODEL_OK && offset == 0);
    CHECK(rfmodel_network_add(network, 2, pad, 4, 75, &offset) == RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_network_port_count(network, &count) == RFMODEL_OK && count == 2);
    CHECK(rfmodel_network_add(network, 2, pad, 4, 50, &offset) == RFMODEL_OK && offset == 2);
    CHECK(rfmodel_network_connect(network, 1, 2) == RFMODEL_OK);
    CHECK(rfmodel_network_external_s(network, external, 2, scattering, 4) == RFMODEL_OK);
    CHECK(fabs(scattering[2].real - 0.25) < 1e-12);
    {
        const double thermal = 1.380649e-23 * 290.;
        rfmodel_complex block[4], intrinsic[16] = {{0, 0}}, result[4] = {{123, 0}};
        size_t i;
        CHECK(rfmodel_passive_noise(2, pad, 4, 290., block, 4) == RFMODEL_OK);
        CHECK(fabs(block[0].real / thermal - 0.75) < 1e-12);
        for (i = 0; i < 4; ++i) {
            intrinsic[i * 4 + i] = block[0];
        }
        CHECK(rfmodel_network_external_noise(network, external, 2, intrinsic, 16, result, 3) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(result[0].real == 123);
        CHECK(rfmodel_network_external_noise(network, external, 2, intrinsic, 16, result, 4) ==
              RFMODEL_OK);
        CHECK(fabs(result[3].real / thermal - 0.9375) < 1e-12);
        CHECK(rfmodel_passive_noise(2, pad, 4, -1., block, 4) == RFMODEL_INVALID_ARGUMENT);
    }
    CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_network_terminate(network, 0, zero, one) == RFMODEL_OK);
    CHECK(rfmodel_network_terminate(network, 3, zero, zero) == RFMODEL_OK);
    CHECK(rfmodel_network_solve(network, incident, outgoing, 3, &residual) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(residual == -1);
    CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) == RFMODEL_OK);
    CHECK(fabs(outgoing[3].real - 0.25) < 1e-12 && residual < 1e-12);
    CHECK(rfmodel_last_error()[0] == '\0');
    rfmodel_network_destroy(network);
    rfmodel_network_destroy(NULL);
    CHECK(rfmodel_network_port_count(NULL, &count) == RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_network_create(50, &network) == RFMODEL_OK);
    {
        const rfmodel_complex through[4] = {{0, 0}, {1, 0}, {1, 0}, {0, 0}};
        CHECK(rfmodel_network_add(network, 2, through, 4, 50, &offset) == RFMODEL_OK);
        CHECK(rfmodel_network_connect(network, 0, 1) == RFMODEL_OK);
        CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) ==
              RFMODEL_SOLVER_ERROR);
        CHECK(strlen(rfmodel_last_error()) > 0);
    }
    rfmodel_network_destroy(network);
    return 0;
}
