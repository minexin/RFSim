// Deliberately linked into the same probe executable, including when consuming
// a relocated SDK. This TU independently includes and invokes the public APIs.
#include <rfmodel/coherent/coherent_amplifier.hpp>
#include <rfmodel/coherent/coherent_compression.hpp>
#include <rfmodel/coherent/source_coherence.hpp>
#include <cmath>
#include <stdexcept>

void rfmodel_reference_source_odr_check() {
    const auto groups = rfmodel::assign_source_coherence({{"second", "clock"}, {"first", "clock"}});
    const std::vector<rfmodel::CoherentComponent> input{
        {10, rfmodel::SpectrumKind::source, 1., groups[0], {.001, .002}},
        {10, rfmodel::SpectrumKind::source, 1., groups[1], {.003, -.001}}};
    const auto shared = rfmodel::compress_coherent_fundamentals(
        1.e8, input, rfmodel::SaturatingFundamentalCompression(20., 20., 23.));
    const auto cubic =
        rfmodel::CoherentLimitedAmplifier(20., 20., 23., 20., 10., 75.).evaluate(1.e8, input, 10);
    if (groups != std::vector<std::uint64_t>({1, 1}) || shared.output.components.size() != 1 ||
        cubic.inputs.size() != 1 || cubic.terms.size() != 4 ||
        !std::isfinite(shared.output.total_power_w) ||
        shared.input_power_w != cubic.total_input_power_w ||
        cubic.inputs.front().amplitude != rfmodel::Complex(.004, .001)) {
        throw std::runtime_error("second translation unit source SDK check");
    }
}
