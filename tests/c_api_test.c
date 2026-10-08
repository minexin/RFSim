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

int main(int argc, char **argv) {
    rfmodel_network *network = NULL;
    const rfmodel_complex pad[4] = {{0, 0}, {0.5, 0}, {0.5, 0}, {0, 0}};
    const rfmodel_complex zero = {0, 0}, one = {1, 0};
    rfmodel_complex incident[4], outgoing[4], scattering[4];
    size_t offset = 999, count = 0;
    const size_t external[2] = {0, 3};
    double residual = -1;
    CHECK(rfmodel_abi_version() == 1);
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_amplifier_term terms[32] = {{99, 98, {97, 96, 95}, {123., 456.}}};
        rfmodel_amplifier_drive drive = {321., 654.};
        size_t written = 777;
        CHECK(rfmodel_multitone_amplifier_terms(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., terms, 15, &written, &drive) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && drive.total_input_power_w == 321. && terms[0].order == 99 &&
              terms[0].contributors[2] == 95 && terms[0].amplitude.real == 123.);
        CHECK(rfmodel_multitone_amplifier_terms(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., terms, 32, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 16 && terms[7].order == 3 && terms[7].index == 10);
        CHECK(terms[7].contributors[0] == -11 && terms[7].contributors[1] == 10 &&
              terms[7].contributors[2] == 11);
        CHECK(fabs(terms[7].amplitude.real / -2e-6 - 1.) < 1e-12);
        CHECK(terms[8].contributors[0] == -10 &&
              fabs(terms[8].amplitude.real / -1e-6 - 1.) < 1e-12);
        CHECK(rfmodel_multitone_amplifier_terms(
                  1e8, NULL, 0, 20., 20., 23., 20., 10., 50., NULL, 0, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 0 && drive.total_input_power_w == 0.);
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_amplifier_component output[32] = {{99, 98, {123., 456.}}};
        rfmodel_amplifier_drive drive = {321., 654.};
        size_t written = 777;
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 1, &written, &drive) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && drive.total_input_power_w == 321. &&
              drive.limited_input_power_w == 654.);
        CHECK(output[0].order == 99 && output[0].index == 98 && output[0].amplitude.real == 123.);
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 32, &written, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && output[0].order == 99);
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 32, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 14 && fabs(drive.total_input_power_w / 2e-6 - 1.) < 1e-12);
        CHECK(output[2].order == 2 && output[2].index == 1);
        CHECK(fabs(output[2].amplitude.real * output[2].amplitude.real / 1e-9 - 1.) < 1e-12);
        CHECK(output[6].order == 3 && output[6].index == 9);
        CHECK(fabs(output[6].amplitude.real / -1e-6 - 1.) < 1e-12);
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, NULL, 0, 20., 20., 23., 20., 10., 50., NULL, 0, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 0 && drive.total_input_power_w == 0. && drive.limited_input_power_w == 0.);
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_spectrum_bin output[3] = {{99, {123., 0.}}};
        size_t written = 777;
        CHECK(rfmodel_single_tone_amplifier_transmit(
                  1e6, input, 1, 20., 20., 23., 20., 10., 50., output, 2, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && output[0].index == 99 && output[0].amplitude.real == 123.);
        CHECK(rfmodel_single_tone_amplifier_transmit(
                  1e6, input, 2, 20., 20., 23., 20., 10., 50., output, 3, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output[0].index == 99);
        CHECK(rfmodel_single_tone_amplifier_transmit(
                  1e6, input, 1, 20., 20., 23., 20., 10., 50., output, 3, &written) == RFMODEL_OK);
        CHECK(written == 3 && output[0].index == 10 && output[1].index == 20 &&
              output[2].index == 30);
        CHECK(fabs(output[1].amplitude.real * output[1].amplitude.real / 2.5e-10 - 1.) < 1e-12);
    }
    {
        rfmodel_complex output = {123., 456.};
        const rfmodel_complex input = {0., .1};
        CHECK(rfmodel_saturating_fundamental(20., 20., 20., input, .01, &output) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456.);
        CHECK(rfmodel_saturating_fundamental(20., 20., 23., input, .001, &output) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456.);
        CHECK(rfmodel_saturating_fundamental(20., 20., 23., input, .01, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_saturating_fundamental(20., 20., 23., input, .01, &output) == RFMODEL_OK);
        CHECK(output.real == 0. &&
              fabs(output.imag * output.imag / .19922937036162172 - 1.) < 1e-12);
    }
    {
        const rfmodel_spectrum_bin input = {10, {.1, 0.}};
        const double coefficients[] = {0., 0., 1.};
        rfmodel_spectrum_bin output[2] = {{123, {456., 0.}}, {789, {0., 0.}}};
        size_t written = 999;
        CHECK(rfmodel_polynomial_amplifier_transmit(
                  1e6, &input, 1, coefficients, 3, 50., output, 1, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output[0].index == 123 && output[0].amplitude.real == 456.);
        CHECK(rfmodel_polynomial_amplifier_transmit(
                  1e6, &input, 1, NULL, 3, 50., output, 2, &written) == RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_polynomial_amplifier_transmit(
                  1e6, &input, 1, coefficients, 3, 50., output, 2, &written) == RFMODEL_OK);
        CHECK(written == 2 && output[0].index == 0 && output[1].index == 20);
        CHECK(fabs(output[0].amplitude.real - sqrt(50.) * .01) < 1e-12);
        CHECK(fabs(output[1].amplitude.real - .05) < 1e-12);
    }
    {
        const rfmodel_spectrum_bin bins[] = {{10, {0., .001}}, {20, {.002, 0.}}};
        const rfmodel_incident_spectrum port = {1e6, bins, 2};
        rfmodel_complex output = {123., 456.};
        double total = 789.;
        CHECK(rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 1, 10, &output, &total) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456. && total == 789.);
        CHECK(rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 0, 10, &output, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456. && total == 789.);
        CHECK(rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 0, 10, &output, &total) ==
              RFMODEL_OK);
        CHECK(fabs(total / 5e-6 - 1.) < 1e-12);
        CHECK(output.real == 0. && output.imag > 0. && output.imag < .01);
    }
    {
        rfmodel_complex output = {123., 456.};
        const rfmodel_complex too_large = {1., 0.};
        const rfmodel_complex point = {0., sqrt(pow(10., -3.9))};
        CHECK(rfmodel_p1db_fundamental(20., 10., too_large, &output) == RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456.);
        CHECK(rfmodel_p1db_fundamental(20., 10., point, NULL) == RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_p1db_fundamental(20., 10., point, &output) == RFMODEL_OK);
        CHECK(fabs(output.imag - .1) < 1e-12 && output.real == 0.);
        {
            const double total = pow(10., -3.9);
            const rfmodel_complex partial = {0., sqrt(total / 4.)};
            CHECK(rfmodel_p1db_driven_fundamental(20., 10., partial, total, &output) == RFMODEL_OK);
            CHECK(fabs(output.imag - .05) < 1e-12 && output.real == 0.);
            CHECK(rfmodel_p1db_driven_fundamental(20., 10., partial, total / 8., &output) ==
                  RFMODEL_INVALID_ARGUMENT);
            CHECK(fabs(output.imag - .05) < 1e-12 && output.real == 0.);
            CHECK(rfmodel_p1db_driven_fundamental(20., 10., partial, total, NULL) ==
                  RFMODEL_INVALID_ARGUMENT);
        }
    }
    {
        rfmodel_touchstone *model = NULL;
        rfmodel_touchstone_info info;
        rfmodel_complex values[4] = {{123., 0.}};
        CHECK(argc == 3);
        CHECK(rfmodel_touchstone_open(argv[1], 2, &model) == RFMODEL_INVALID_ARGUMENT);
        CHECK(model == NULL);
        CHECK(rfmodel_touchstone_open(argv[1], 0, &model) == RFMODEL_OK);
        CHECK(rfmodel_touchstone_get_info(model, &info) == RFMODEL_OK);
        CHECK(info.ports == 2 && info.reference_ohms == 50. && info.noise_sample_count == 0);
        CHECK(info.minimum_frequency_hz == 1e9 && info.maximum_frequency_hz == 3e9);
        CHECK(rfmodel_touchstone_s(model, 2e9, 50., values, 3) == RFMODEL_INVALID_ARGUMENT);
        CHECK(values[0].real == 123.);
        CHECK(rfmodel_touchstone_s(model, 4e9, 50., values, 4) == RFMODEL_INVALID_ARGUMENT);
        CHECK(values[0].real == 123.);
        CHECK(rfmodel_touchstone_s(model, 2e9, 50., values, 4) == RFMODEL_OK);
        CHECK(fabs(values[2].real - 0.375) < 1e-12);
        rfmodel_touchstone_close(model);
        CHECK(rfmodel_touchstone_open(argv[2], 0, &model) == RFMODEL_OK);
        values[0].real = 123.;
        CHECK(rfmodel_touchstone_noise(model, 2e9, 50., 290., values, 3) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(values[0].real == 123.);
        CHECK(rfmodel_touchstone_noise(model, 2e9, 50., 290., values, 4) == RFMODEL_OK);
        CHECK(fabs(values[0].real / (1.380649e-23 * 290.) - .96) < 1e-12);
        CHECK(fabs(values[1].real / (1.380649e-23 * 290.) + .384) < 1e-12);
        rfmodel_touchstone_close(model);
        rfmodel_touchstone_close(NULL);
    }
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
    {
        const rfmodel_spectrum_bin input = {7, {0., 2.}};
        rfmodel_spectrum_bin output = {999, {123., 0.}};
        size_t produced = 999;
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 2, 1e6, &input, 1, &output, 0, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(produced == 999 && output.index == 999 && output.amplitude.real == 123.);
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 2, 1e6, &input, 1, &output, 1, &produced) == RFMODEL_OK);
        CHECK(produced == 1 && output.index == 7 && fabs(output.amplitude.imag - 0.5) < 1e-12);
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 2, 1e6, NULL, 0, NULL, 0, &produced) == RFMODEL_OK);
        CHECK(produced == 0);
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 1, 1e6, NULL, 0, NULL, 0, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
    }
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
