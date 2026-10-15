#pragma once

#include "rfmodel/core/device_model.hpp"
#include "rfmodel/mna/linear_network.hpp"

namespace rfmodel::mna {

// Oriented terminal pair. Voltage is v[p]-v[n]; current enters the DUT at p.
// Unlike circuit elements, ports may have identical terminals, repeat another
// pair, reverse its orientation, or have linearly dependent incidence columns.
struct Port {
    NodeId p;
    NodeId n;
};

struct PowerWaveSResult {
    rfmodel::SMatrix scattering;
    std::vector<rfmodel::Complex> references_ohm;
    // Column j describes the solve with unit incident wave at port j.
    std::vector<Diagnostics> column_diagnostics;
};

// Single strictly positive frequency; RMS Kurokawa waves:
// a=(V+Z I)/(2 sqrt(Re Z)), b=(V-conj(Z) I)/(2 sqrt(Re Z)).
// Independent sources are deactivated incrementally; voltage constraints and
// their branch currents remain. Requires a unique full loaded state, not merely
// unique observable port waves. No bare-network DC/AC solve is required.
inline PowerWaveSResult extract_power_wave_s(const Network &network,
                                             double frequency_hz,
                                             const std::vector<Port> &ports,
                                             const std::vector<rfmodel::Complex> &references_ohm,
                                             const SolveOptions &options = {}) {
    detail::validate_options(options);
    if (!detail::positive(frequency_hz) || ports.empty() || ports.size() != references_ohm.size()) {
        detail::fail(Failure::invalid_input, "MNA invalid power-wave frequency or port count");
    }
    const auto count = ports.size();
    // Guard all dimensions before allocation. Assembly applies the stronger
    // bound on internal unknowns plus ports; these checks also cover count^2.
    if (count > 256 || count > std::numeric_limits<std::size_t>::max() / count) {
        detail::fail(Failure::budget_exceeded, "MNA power-wave port budget exceeded");
    }
    for (std::size_t k = 0; k < count; ++k) {
        if (ports[k].p.value >= network.node_count() || ports[k].n.value >= network.node_count() ||
            !detail::positive(references_ohm[k].real()) || !detail::valid(references_ohm[k])) {
            detail::fail(Failure::invalid_input, "MNA invalid power-wave terminal or reference");
        }
    }
    const double omega = detail::product(6.283185307179586476925286766559, frequency_hz);
    auto assembled =
        detail::assemble(network, true, omega, detail::SourceMode::incremental_zero, count);
    const auto n = assembled.unknowns, base = assembled.internal_unknowns;
    const auto stamp = [&](std::size_t row, std::size_t column, Complex value) {
        auto &entry = assembled.matrix[row * n + column];
        entry = detail::checked(entry + value);
    };
    std::vector<double> denominators(count);
    for (std::size_t k = 0; k < count; ++k) {
        const auto current = base + k;
        // Add, never assign: same-terminal contributions cancel exactly.
        if (ports[k].p.value) {
            stamp(ports[k].p.value - 1, current, -1);
            stamp(current, ports[k].p.value - 1, 1);
        }
        if (ports[k].n.value) {
            stamp(ports[k].n.value - 1, current, 1);
            stamp(current, ports[k].n.value - 1, -1);
        }
        stamp(current, current, references_ohm[k]);
        denominators[k] = detail::product(2, std::sqrt(references_ohm[k].real()));
    }
    // Only reference connectivity includes port loads. Port edges never enter
    // the exact ideal-voltage-source-cycle test, including duplicates/reversals.
    detail::validate_topology(network, true, [&](const auto &join) {
        for (const auto &port : ports) {
            join(port.p, port.n);
        }
    });
    const auto factor =
        detail::factor_matrix(assembled.matrix, n, assembled.voltage_unknowns, options);
    PowerWaveSResult result{
        {count, std::vector<rfmodel::Complex>(count * count)}, references_ohm, {}};
    result.column_diagnostics.reserve(count);
    // One factorization; each column has its own scaling, residual checks and
    // bounded refinement. No internal element-power products are needed for S.
    for (std::size_t column = 0; column < count; ++column) {
        std::vector<Complex> rhs(n);
        rhs[base + column] = denominators[column];
        const auto solved = detail::solve_factored(factor, rhs);
        for (std::size_t row = 0; row < count; ++row) {
            const auto p = ports[row].p.value, q = ports[row].n.value;
            const auto voltage = detail::checked((p ? solved.x[p - 1] : Complex{}) -
                                                 (q ? solved.x[q - 1] : Complex{}));
            const auto reflected = detail::checked(
                voltage - detail::multiply(std::conj(references_ohm[row]), solved.x[base + row]));
            result.scattering(row, column) = detail::divide(reflected, denominators[row]);
        }
        result.column_diagnostics.push_back(solved.diagnostics);
    }
    return result;
}
} // namespace rfmodel::mna
