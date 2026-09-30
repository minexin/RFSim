#include <rfmodel/c_api.h>
#include <math.h>

int main(int argc, char **argv) {
    rfmodel_network *network = NULL;
    const rfmodel_complex s[4] = {{0, 0}, {0, -0.5}, {0, -0.5}, {0, 0}};
    const size_t ports[2] = {0, 1};
    rfmodel_complex result[4];
    size_t offset;
    int status;
    {
        const rfmodel_spectrum_bin bins[] = {{10, {.001, 0.}}, {20, {.002, 0.}}};
        const rfmodel_incident_spectrum port = {1e6, bins, 2};
        rfmodel_complex output;
        double total;
        if (rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 0, 10, &output, &total) !=
                RFMODEL_OK ||
            fabs(total / 5e-6 - 1.) > 1e-12 || output.real <= 0. || output.real >= .01) {
            return 14;
        }
    }
    {
        const rfmodel_complex input = {0., sqrt(pow(10., -3.9))};
        rfmodel_complex output;
        if (rfmodel_p1db_fundamental(20., 10., input, &output) != RFMODEL_OK ||
            fabs(output.imag - .1) > 1e-12) {
            return 12;
        }
        {
            const rfmodel_complex partial = {0., input.imag / 2.};
            if (rfmodel_p1db_driven_fundamental(20., 10., partial, pow(10., -3.9), &output) !=
                    RFMODEL_OK ||
                fabs(output.imag - .05) > 1e-12) {
                return 13;
            }
        }
    }
    {
        rfmodel_touchstone *model = NULL;
        rfmodel_complex reflection;
        if (argc != 2 || rfmodel_touchstone_open(argv[1], 0, &model) != RFMODEL_OK) {
            return 9;
        }
        status = rfmodel_touchstone_s(model, 1e6, 50., &reflection, 1);
        {
            rfmodel_complex absent_noise = {123., 0.};
            if (rfmodel_touchstone_noise(model, 1e6, 50., 290., &absent_noise, 1) !=
                    RFMODEL_INVALID_ARGUMENT ||
                absent_noise.real != 123.) {
                rfmodel_touchstone_close(model);
                return 11;
            }
        }
        rfmodel_touchstone_close(model);
        if (status != RFMODEL_OK || fabs(reflection.real - 0.2) > 1e-12) {
            return 10;
        }
    }
    {
        const rfmodel_complex zero_noise = {0., 0.}, intrinsic = {1., 0.}, gamma = {0.5, 0.};
        rfmodel_complex incident_noise, outgoing_noise;
        double net;
        if (rfmodel_loaded_noise(1,
                                 &zero_noise,
                                 &intrinsic,
                                 &zero_noise,
                                 1,
                                 &gamma,
                                 1,
                                 &incident_noise,
                                 &outgoing_noise,
                                 1,
                                 &net,
                                 1) != RFMODEL_OK ||
            fabs(net + 0.75) > 1e-12) {
            return 8;
        }
    }
    {
        const rfmodel_spectrum_bin input = {10, {1., 0.}};
        rfmodel_spectrum_bin output[2];
        size_t produced;
        if (rfmodel_ideal_mixer_transmit(1e6, &input, 1, 2, 0., 0., 50., output, 2, &produced) !=
                RFMODEL_OK ||
            produced != 2 || output[0].index != 8 || fabs(output[0].amplitude.real - 1.) > 1e-12) {
            return 7;
        }
    }
    {
        const rfmodel_complex matched = {50., 0.};
        if (rfmodel_linear_amplifier_s(1e9, 20., 90., 50., 0., matched, matched, 50., result, 4) !=
                RFMODEL_OK ||
            fabs(result[2].imag - 10.) > 1e-12) {
            return 6;
        }
    }
    if (rfmodel_rlgc_line_s(0., 25., 0., 0., 0., 2., 50., result, 4) != RFMODEL_OK ||
        fabs(result[2].real - 2. / 3.) > 1e-12) {
        return 4;
    }
    if (rfmodel_transmission_line_s(1e9, 100., 0.25e-9, 0., 50., result, 4) != RFMODEL_OK ||
        fabs(result[0].real - 0.6) > 1e-12) {
        return 5;
    }
    if (rfmodel_abi_version() != 1 || rfmodel_network_create(50., &network) != RFMODEL_OK) {
        return 1;
    }
    status = rfmodel_network_add(network, 2, s, 4, 50., &offset);
    if (status == RFMODEL_OK) {
        status = rfmodel_network_external_s(network, ports, 2, result, 4);
    }
    if (status == RFMODEL_OK) {
        rfmodel_complex covariance[4], output_noise[4];
        status = rfmodel_passive_noise(2, s, 4, 290., covariance, 4);
        if (status == RFMODEL_OK) {
            status =
                rfmodel_network_external_noise(network, ports, 2, covariance, 4, output_noise, 4);
            if (status == RFMODEL_OK &&
                fabs(output_noise[0].real / (1.380649e-23 * 290.) - 0.75) > 1e-12) {
                status = RFMODEL_INTERNAL_ERROR;
            }
        }
    }
    if (status == RFMODEL_OK) {
        const rfmodel_spectrum_bin input = {1, {2., 0.}};
        rfmodel_spectrum_bin output;
        size_t produced = 0;
        status = rfmodel_network_transmit_spectrum(
            network, ports, 2, 1e6, &input, 1, &output, 1, &produced);
        if (status == RFMODEL_OK &&
            (produced != 1 || output.index != 1 || fabs(output.amplitude.imag + 1.) > 1e-12)) {
            status = RFMODEL_INTERNAL_ERROR;
        }
    }
    rfmodel_network_destroy(network);
    if (status != RFMODEL_OK) {
        return 2;
    }
    return fabs(result[2].imag + 0.5) < 1e-12 && fabs(result[2].real) < 1e-12 ? 0 : 3;
}
