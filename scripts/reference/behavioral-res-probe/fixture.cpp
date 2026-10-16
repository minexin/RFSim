#include "fixture.hpp"

namespace res_probe {
namespace {
rfmodel::ModelAdapter resistor(const char *name) {
    // Analytic single RES at equal real references; shared by both analyses.
    const double denominator = resistance_ohms + 2. * reference_ohms;
    const double reflection = resistance_ohms / denominator;
    const double transmission = 2. * reference_ohms / denominator;
    rfmodel::StaticSParameterModel model(
        name, {{0, "p0", reference_ohms}, {1, "p1", reference_ohms}},
        {2, {reflection, transmission, transmission, reflection}});
    return rfmodel::adapt_device_model(std::move(model),
        {"res_probe.static_s_ideal_series_resistor", "1", "", ""});
}
std::size_t global_port(const Fixture &f, std::size_t device, std::size_t port) {
    if (port >= f.models.at(device).descriptor().ports.size())
        throw std::invalid_argument("invalid fixture port");
    std::size_t offset = 0;
    for (std::size_t d = 0; d < device; ++d)
        offset += f.models.at(d).descriptor().ports.size();
    return offset + port;
}
} // namespace
Fixture fixture() {
    return {{resistor("R1"), resistor("R2")}, {{0, 1, 1, 0}}, {0, 3}};
}
rfmodel::LinearNetwork linear_network(const Fixture &f, double frequency) {
    rfmodel::LinearNetwork network(reference_ohms);
    for (const auto &model : f.models) model.add_to(network, frequency);
    for (const auto &wire : f.connections)
        network.connect(global_port(f, wire.first_device, wire.first_port),
                        global_port(f, wire.second_device, wire.second_port));
    return network;
}
rfmodel::ConversionSimulationRequest loaded_request(const Fixture &f) {
    rfmodel::ConversionSimulationRequest request;
    for (const auto &model : f.models)
        request.devices.push_back(model.conversion_device(
            frequency_hz, {1}, rfmodel::ModelNoisePolicy::omit_intrinsic_noise));
    request.connections = f.connections;
    request.devices.at(0).source.at(0) = std::sqrt(source_available_w);
    // All boundary reflections are zero: physical source/load are real 50 ohm.
    // Empty measurement/noise requests do not imply a vendor noise validation.
    return request;
}
} // namespace res_probe
