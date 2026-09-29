#include <rfmodel/c_api.h>
#include <math.h>

int main(void) {
    rfmodel_network *network = NULL;
    const rfmodel_complex s[4] = {{0, 0}, {0, -0.5}, {0, -0.5}, {0, 0}};
    const size_t ports[2] = {0, 1};
    rfmodel_complex result[4];
    size_t offset;
    int status;
    if (rfmodel_abi_version() != 1 || rfmodel_network_create(50., &network) != RFMODEL_OK) {
        return 1;
    }
    status = rfmodel_network_add(network, 2, s, 4, 50., &offset);
    if (status == RFMODEL_OK) {
        status = rfmodel_network_external_s(network, ports, 2, result, 4);
    }
    rfmodel_network_destroy(network);
    if (status != RFMODEL_OK) {
        return 2;
    }
    return fabs(result[2].imag + 0.5) < 1e-12 && fabs(result[2].real) < 1e-12 ? 0 : 3;
}
