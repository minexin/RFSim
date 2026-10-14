#include <rfmodel/conversion/conversion_network.hpp>
#include <rfmodel/conversion/conversion_power_wave.hpp>
#include <cmath>

// A second translation unit independently instantiates public inline API code.
// Linking it with conversion_api.cpp catches header-only ODR/linkage mistakes.
bool conversion_api_odr_probe() {
    const rfmodel::FrequencyConversionModel original(1e9, {{0, 1}}, {1, {.2}}, {1, {.1}});
    const auto change = rfmodel::renormalize_power_waves(original, {{75., 10.}});
    const auto boundary = rfmodel::renormalize_conversion_boundary(
        original, {{75., 10.}}, {0}, {rfmodel::Complex{.01, .02}}, {0.}, original.zero_noise());
    rfmodel::FrequencyConversionNetwork network(1e9);
    network.add_power_waves({change.model,
                             boundary.source,
                             boundary.reflection,
                             boundary.source_noise,
                             original.zero_noise()});
    const auto full = network.analyze(true);
    const rfmodel::ConversionWaveResult result = network.solve_waves();
    const auto model_waves = change.model.solve_waves(boundary.source, boundary.reflection);
    if (std::abs(result.outgoing[0] - full.outgoing[0]) > 1e-12 ||
        std::abs(model_waves.outgoing[0] - result.outgoing[0]) > 1e-12 ||
        result.relative_residual > 1e-12) {
        return false;
    }
    const rfmodel::Complex z{75., 10.};
    const auto voltage =
        (std::conj(z) * result.incident[0] + z * result.outgoing[0]) / std::sqrt(z.real());
    const auto current = (result.incident[0] - result.outgoing[0]) / std::sqrt(z.real());
    const auto old_outgoing = (voltage - 50. * current) / (2. * std::sqrt(50.));
    return std::abs(old_outgoing - rfmodel::Complex{.003, .002}) < 1e-12;
}
