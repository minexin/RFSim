#include <rfmodel/noise/phase_jitter.hpp>
#include <cmath>

rfmodel::PhaseJitterMeasurement phase_jitter_probe();

int main() {
    const auto result = phase_jitter_probe();
    const rfmodel::PhaseJitterRequest request{
        {{1e3, 1e-12}, {1e6, 1e-12}},
        1e8,
        1e3,
        1e6,
        rfmodel::PhaseJitterIntegrationPolicy::linear_rad2_per_hz};
    const auto local = rfmodel::measure_phase_jitter(request);
    return result.phase_variance_rad2 == local.phase_variance_rad2 &&
                   result.rms_time_s == local.rms_time_s &&
                   std::abs(result.rms_time_s / 1.5907534571602809633e-12 - 1.) < 4e-14
               ? 0
               : 1;
}
