#include <rfmodel/phase_jitter.hpp>

rfmodel::PhaseJitterMeasurement phase_jitter_probe() {
    const rfmodel::PhaseJitterRequest request{
        {{1e3, 1e-12}, {1e6, 1e-12}},
        1e8,
        1e3,
        1e6,
        rfmodel::PhaseJitterIntegrationPolicy::linear_rad2_per_hz};
    return rfmodel::measure_phase_jitter(request);
}
