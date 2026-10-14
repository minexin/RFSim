#include <rfmodel/conversion/conversion_simulation.hpp>
#include <cmath>
#include <stdexcept>

bool operating_noise_probe();

int main() {
    using namespace rfmodel;
    FrequencyConversionModel model(
        1e6, {{0, 1}}, {1, {0.}}, {1, {0.}}, std::vector<Complex>{{37., 11.}});
    ConversionSimulationRequest request;
    request.devices.push_back({model, {2.}, {0.}, model.zero_noise(), model.zero_noise()});
    request.operating_point = ConversionOperatingOptions{};
    int calls = 0;
    request.intrinsic_noise_providers.push_back(
        {0, [&](const ConversionAcceptedState &state) {
             ++calls;
             auto noise = state.jacobian.zero_noise();
             noise.covariance(0, 0) = 1e-18 * (1. + std::norm(state.incident[0]));
             noise.complementary(0, 0) = Complex{1e-18, 2e-18};
             return ConversionIntrinsicNoiseSnapshot{state.jacobian.spacing_hz(),
                                                     state.jacobian.channels(),
                                                     state.jacobian.reference_impedances_ohms(),
                                                     noise};
         }});
    auto result = analyze_conversion_simulation(request);
    if (calls != 1 ||
        std::abs(result.waves.outgoing_noise.covariance(0, 0).real() / 5e-18 - 1.) > 1e-12 ||
        result.accepted_devices[0].intrinsic_noise.complementary(0, 0) != Complex{1e-18, 2e-18} ||
        !operating_noise_probe()) {
        throw std::runtime_error("installed accepted-state noise consumer failed");
    }
}
