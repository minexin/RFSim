#include <rfmodel/conversion/conversion_phase_response.hpp>

rfmodel::ConversionNoise phase_response_odr_probe() {
    using namespace rfmodel;
    const auto z = conversion_detail::zero(3);
    const FrequencyConversionModel m(1., {{0, 9}, {0, 10}, {0, 11}}, z, z);
    ConversionSimulationRequest request;
    request.devices = {{m, {0., 1., 0.}, {0., 0., 0.}, m.zero_noise(), m.zero_noise()}};
    const auto ready = with_phase_response_noise(
        request, {{"explicit shared reference", {{{0, 0, 10}}}, {{1, 1., 2e-10, {{0., 1.}}}}}});
    return *ready.additional_source_noise;
}
