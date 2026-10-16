// Real second TU, linked with the probe in source and relocated SDK builds.
#include <rfmodel/coherent/coherent_pipeline.hpp>
#include <cmath>
#include <stdexcept>
void rfmodel_reference_cascade_odr_check() {
    rfmodel::CoherentPipeline p(1.e8, 50., 7);
    const auto seed = p.seed({{10, rfmodel::SpectrumKind::source, 1., 7, {.001,.002}}});
    const rfmodel::CoherentLimitedAmplifier m(10.,20.,23.,30.,20.);
    const auto a = p.limited_amplifier(seed,m);
    const auto b = p.limited_amplifier(a.output,m,rfmodel::AmplifierInputMode::propagate_distortion);
    if (p.inspect(a.output).records.size()!=4 || p.inspect(b.output).records.size()!=4 ||
        !std::isfinite(b.input_power_w) || p.physical_root(1).coherence_group!=7) {
        throw std::runtime_error("second TU cascade SDK check");
    }
}
