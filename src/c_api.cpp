#include "rfmodel/c_api.h"
#include "rfmodel/network.hpp"
#include <cstdio>
#include <new>

struct rfmodel_network {
    rfmodel::LinearNetwork core;
    size_t ports{};

    explicit rfmodel_network(double reference) : core(reference) {
    }
};

namespace {
thread_local char last_error[512]{};

template <class Action> int guarded(Action action) noexcept {
    last_error[0] = '\0';
    try {
        action();
        return RFMODEL_OK;
    } catch (const std::bad_alloc &) {
        std::snprintf(last_error, sizeof(last_error), "allocation failed");
        return RFMODEL_OUT_OF_MEMORY;
    } catch (const std::invalid_argument &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_INVALID_ARGUMENT;
    } catch (const std::out_of_range &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_INVALID_ARGUMENT;
    } catch (const std::runtime_error &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_SOLVER_ERROR;
    } catch (...) {
        std::snprintf(last_error, sizeof(last_error), "unexpected internal error");
        return RFMODEL_INTERNAL_ERROR;
    }
}

void require(bool condition) {
    if (!condition) {
        throw std::invalid_argument("null pointer, invalid count or insufficient buffer");
    }
}
} // namespace

extern "C" {
const char *rfmodel_last_error(void) {
    return last_error;
}

unsigned int rfmodel_abi_version(void) {
    return 1;
}

int rfmodel_network_create(double reference, rfmodel_network **out) {
    return guarded([&] {
        require(out != nullptr);
        *out = nullptr;
        *out = new rfmodel_network(reference);
    });
}

void rfmodel_network_destroy(rfmodel_network *network) {
    delete network;
}

int rfmodel_network_add(rfmodel_network *network,
                        size_t ports,
                        const rfmodel_complex *values,
                        size_t value_count,
                        double reference,
                        size_t *first_port) {
    return guarded([&] {
        require(network && values && first_port && ports > 0 && ports <= 1024);
        require(value_count == ports * ports);
        rfmodel::SMatrix matrix{ports, std::vector<rfmodel::Complex>(value_count)};
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {values[i].real, values[i].imag};
        }
        // Commit only after success, including allocation failures inside core.add().
        auto updated = network->core;
        const auto offset = updated.add(matrix, reference);
        network->core = std::move(updated);
        network->ports += ports;
        *first_port = offset;
    });
}

int rfmodel_network_connect(rfmodel_network *network, size_t first, size_t second) {
    return guarded([&] {
        require(network != nullptr);
        network->core.connect(first, second);
    });
}

int rfmodel_network_terminate(rfmodel_network *network,
                              size_t port,
                              rfmodel_complex reflection,
                              rfmodel_complex source) {
    return guarded([&] {
        require(network != nullptr);
        network->core.terminate(
            port, {reflection.real, reflection.imag}, {source.real, source.imag});
    });
}

int rfmodel_network_port_count(const rfmodel_network *network, size_t *count) {
    return guarded([&] {
        require(network && count);
        *count = network->ports;
    });
}

int rfmodel_network_solve(const rfmodel_network *network,
                          rfmodel_complex *incident,
                          rfmodel_complex *outgoing,
                          size_t capacity,
                          double *relative_residual) {
    return guarded([&] {
        require(network && incident && outgoing && relative_residual);
        require(capacity >= network->ports);
        const auto waves = network->core.solve();
        for (size_t i = 0; i < network->ports; ++i) {
            incident[i] = {waves.incident[i].real(), waves.incident[i].imag()};
            outgoing[i] = {waves.outgoing[i].real(), waves.outgoing[i].imag()};
        }
        *relative_residual = waves.relative_residual;
    });
}

int rfmodel_network_external_s(const rfmodel_network *network,
                               const size_t *ports,
                               size_t port_count,
                               rfmodel_complex *values,
                               size_t capacity) {
    return guarded([&] {
        require(network && ports && values && port_count > 0 && port_count <= 1024);
        require(capacity >= port_count * port_count);
        const auto matrix =
            network->core.external_s(std::vector<size_t>(ports, ports + port_count));
        for (size_t i = 0; i < matrix.values.size(); ++i) {
            values[i] = {matrix.values[i].real(), matrix.values[i].imag()};
        }
    });
}
} // extern "C"
