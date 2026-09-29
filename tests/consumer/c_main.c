#include <rfmodel/c_api.h>
#include <math.h>

int main(void) {
    rfmodel_network *network = NULL;
    const rfmodel_complex s[4] = {{0, 0}, {0, -0.5}, {0, -0.5}, {0, 0}};
    const size_t ports[2] = {0, 1};
    rfmodel_complex result[4];
    size_t offset;
    int status;
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
    rfmodel_network_destroy(network);
    if (status != RFMODEL_OK) {
        return 2;
    }
    return fabs(result[2].imag + 0.5) < 1e-12 && fabs(result[2].real) < 1e-12 ? 0 : 3;
}
