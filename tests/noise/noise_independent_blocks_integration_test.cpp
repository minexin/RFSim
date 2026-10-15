#include "rfmodel/mna/power_wave_s.hpp"
#include "rfmodel/linear/network.hpp"
#include "rfmodel/noise/loaded_noise.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace rfmodel;

namespace {
namespace mna = rfmodel::mna;
constexpr double boltzmann = 1.380649e-23;
constexpr double resistance = 50.;
using Order = std::array<std::size_t, 2>;

SMatrix zero(std::size_t n) {
    return {n, std::vector<Complex>(n * n)};
}

SMatrix external_pair(const SMatrix &full) {
    return {2, {full(0, 0), full(0, 1), full(1, 0), full(1, 1)}};
}

void density_near(Complex actual,
                  double expected,
                  double relative_tolerance,
                  const std::string &context) {
    // Scale every assertion by this port's physical density, never by a global
    // covariance norm or a unit absolute floor that could hide a missing source.
    const bool finite = std::isfinite(actual.real()) && std::isfinite(actual.imag());
    if (!finite || expected <= 0. || actual.real() <= 0. ||
        std::abs(actual - expected) > relative_tolerance * expected) {
        std::ostringstream message;
        message << std::setprecision(17) << context << ": actual=" << actual
                << ", expected=" << expected << ", relative tolerance=" << relative_tolerance;
        throw std::runtime_error(message.str());
    }
}

struct AuxiliaryResult {
    SMatrix full;
    // Indexed by physical resistor, irrespective of auxiliary insertion order.
    std::array<SMatrix, 2> isolated;
};

AuxiliaryResult loaded_route(const SMatrix &scattering,
                             const std::array<double, 2> &emission,
                             const Order &auxiliary_order) {
    const std::vector<Complex> reflections{0., 0., 1., 1.};
    const NoiseCorrelation noiseless{zero(4)};
    auto full_emission = zero(4);
    for (std::size_t slot = 0; slot < 2; ++slot) {
        full_emission(2 + slot, 2 + slot) = emission[slot];
    }
    const auto full = loaded_noise(scattering, noiseless, reflections, {full_emission});
    AuxiliaryResult result{external_pair(full.outgoing.watts_per_hz), {zero(2), zero(2)}};
    for (std::size_t port = 0; port < 2; ++port) {
        require(full.incident.watts_per_hz(port, port) == Complex{},
                "matched external MNA port has nonzero incident noise");
        require(full.net_into_device_w_per_hz[port] ==
                    -full.outgoing.watts_per_hz(port, port).real(),
                "external MNA noise power balance mismatch");
    }
    for (std::size_t slot = 0; slot < 2; ++slot) {
        auto isolated_emission = zero(4);
        isolated_emission(2 + slot, 2 + slot) = emission[slot];
        result.isolated[auxiliary_order[slot]] =
            external_pair(loaded_noise(scattering, noiseless, reflections, {isolated_emission})
                              .outgoing.watts_per_hz);
    }
    return result;
}

AuxiliaryResult network_route(const SMatrix &scattering,
                              const std::vector<Complex> &references,
                              const std::array<double, 2> &emission,
                              const Order &auxiliary_order) {
    LinearNetwork network;
    network.add_power_waves(scattering, references);
    std::vector<NoiseCorrelation> source_blocks{{zero(4)}};
    for (std::size_t slot = 0; slot < 2; ++slot) {
        // A one-port open source makes a_aux=b_aux+e, matching Gamma=1
        // in the loaded route. Its explicit covariance is R_aux*4*k*T/R.
        const auto source = network.add_power_waves({1, {1.}}, {references[2 + slot]});
        network.connect(2 + slot, source);
        source_blocks.push_back({{1, {emission[slot]}}});
    }
    AuxiliaryResult result{
        network.external_noise({0, 1}, independent_noise(source_blocks)).watts_per_hz,
        {zero(2), zero(2)}};
    const auto budgets = network.external_noise_contributions({0, 1}, source_blocks);
    require(budgets.size() == 3, "MNA source-block budget count mismatch");
    for (auto value : budgets[0].watts_per_hz.values) {
        require(value == Complex{}, "noiseless MNA scattering block emitted noise");
    }
    for (std::size_t slot = 0; slot < 2; ++slot) {
        result.isolated[auxiliary_order[slot]] = budgets[1 + slot].watts_per_hz;
    }
    return result;
}

void check_physical_result(const AuxiliaryResult &result,
                           const std::array<double, 2> &temperatures,
                           const std::array<Complex, 2> &external_references,
                           const Order &external_order,
                           double physical_tolerance,
                           const std::string &context) {
    require(result.full.ports == 2, "MNA external noise dimensions changed");
    for (std::size_t row = 0; row < 2; ++row) {
        const auto physical = external_order[row];
        const auto reference = external_references[physical];
        // Independent V/I oracle: (1/R+1/Z)*V=j and
        // b=sqrt(Re Z)*V/Z, with E[j*j^H]=4*k*T/R.
        // This uses no library noise, S-renormalization, or linear-solve helper.
        const double expected = 4. * boltzmann * temperatures[physical] * resistance *
                                reference.real() / std::norm(resistance + reference);
        density_near(result.full(row, row), expected, physical_tolerance, context + " full");
        density_near(result.isolated[physical](row, row),
                     expected,
                     physical_tolerance,
                     context + " isolated physical source");
        require(result.isolated[1 - physical](row, row) == Complex{},
                "independent MNA resistor received another source's noise");
        const double budget =
            result.isolated[0](row, row).real() + result.isolated[1](row, row).real();
        density_near(result.full(row, row), budget, 2e-14, context + " source budget closure");
    }
    // These networks are exactly disconnected, not approximately independent.
    // Exact zeros matter to this narrow original-component retention contract.
    for (const auto *covariance : {&result.full, &result.isolated[0], &result.isolated[1]}) {
        require((*covariance)(0, 1) == Complex{} && (*covariance)(1, 0) == Complex{},
                "independent MNA resistors acquired cross correlation");
    }
}

void independent_resistor_reference_cases() {
    mna::NetworkBuilder builder;
    const std::array<mna::NodeId, 2> nodes{builder.add_node(), builder.add_node()};
    builder.add_resistor(nodes[0], mna::ground, resistance);
    builder.add_resistor(nodes[1], mna::ground, resistance);
    const auto circuit = builder.finalize();
    const std::array<Order, 2> orders{{{{0, 1}}, {{1, 0}}}};
    const std::array<std::array<double, 2>, 2> temperatures{{{{300., 300.}}, {{77., 417.}}}};
    const std::array<std::array<Complex, 2>, 2> external_references{
        {{{50., 50.}}, {{Complex{50., 20.}, Complex{50., -10.}}}}};
    const std::array<std::array<double, 2>, 2> auxiliary_references{{{{50., 50.}}, {{1e8, 1e-6}}}};

    for (const auto &temperature : temperatures) {
        for (const auto &external_reference : external_references) {
            for (const auto &external_order : orders) {
                for (const auto &auxiliary_order : orders) {
                    for (const auto &auxiliary_reference : auxiliary_references) {
                        std::vector<mna::Port> ports;
                        std::vector<Complex> references;
                        for (auto physical : external_order) {
                            ports.push_back({nodes[physical], mna::ground});
                            references.push_back(external_reference[physical]);
                        }
                        std::array<double, 2> emission{};
                        for (std::size_t slot = 0; slot < 2; ++slot) {
                            const auto physical = auxiliary_order[slot];
                            ports.push_back({nodes[physical], mna::ground});
                            references.push_back(auxiliary_reference[slot]);
                            emission[slot] = auxiliary_reference[slot] * 4. * boltzmann *
                                             temperature[physical] / resistance;
                        }
                        const auto scattering =
                            mna::extract_power_wave_s(circuit, 1., ports, references).scattering;
                        const auto loaded = loaded_route(scattering, emission, auxiliary_order);
                        const auto network =
                            network_route(scattering, references, emission, auxiliary_order);
                        const bool extreme = auxiliary_reference[0] == 1e8;
                        // The extreme-reference feedback already has ~2e-9
                        // physical error for an isolated source. Recovery is not
                        // a new machine-precision or arbitrary-reference claim.
                        const double physical_tolerance = extreme ? 3e-9 : 2e-13;
                        std::ostringstream context;
                        context << "T=" << temperature[0] << ',' << temperature[1]
                                << " external Z=" << external_reference[0] << ','
                                << external_reference[1] << " external first=" << external_order[0]
                                << " auxiliary first=" << auxiliary_order[0]
                                << " auxiliary Z=" << auxiliary_reference[0] << ','
                                << auxiliary_reference[1];
                        check_physical_result(loaded,
                                              temperature,
                                              external_reference,
                                              external_order,
                                              physical_tolerance,
                                              context.str() + " loaded");
                        check_physical_result(network,
                                              temperature,
                                              external_reference,
                                              external_order,
                                              physical_tolerance,
                                              context.str() + " network");
                    }
                }
            }
        }
    }
}
} // namespace

int main() {
    try {
        independent_resistor_reference_cases();
        std::cout << "Independent MNA noise physical and source-budget regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
