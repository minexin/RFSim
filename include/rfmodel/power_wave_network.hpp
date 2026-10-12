#pragma once
#include "network.hpp"
#include "power_wave_reference.hpp"

namespace rfmodel {
// Signal-only external reduction at one frequency. Selected ports become 0..N-1
// in the supplied order. Internal physical connections and passive terminations
// are eliminated before changing the external power-wave coordinates. Intrinsic
// noise is supplied separately to LinearNetwork and is not stored in this result.
inline LinearNetwork reduce_power_wave_network(const LinearNetwork &network,
                                               const std::vector<std::size_t> &external_ports,
                                               double reference_ohms) {
    LinearNetwork reduced(reference_ohms);
    auto scattering = network.external_s(external_ports);
    std::vector<Complex> references;
    bool unchanged = true;
    for (auto port : external_ports) {
        const auto reference = network.port_reference_impedance_ohms(port);
        references.push_back(reference);
        unchanged = unchanged && reference == Complex{reference_ohms, 0.};
    }
    if (!unchanged) {
        scattering =
            renormalize_power_waves(
                scattering, references, std::vector<Complex>(external_ports.size(), reference_ohms))
                .scattering;
    }
    reduced.add(scattering, reference_ohms);
    return reduced;
}
} // namespace rfmodel
