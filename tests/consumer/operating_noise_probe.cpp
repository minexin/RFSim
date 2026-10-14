#include <rfmodel/conversion/conversion_operating_point.hpp>
#include <type_traits>

bool operating_noise_probe() {
    using namespace rfmodel;
    // Original names remain unambiguous and preserve their complete function types.
    auto legacy = &solve_conversion_operating_point;
    auto power = &solve_conversion_operating_point_power_waves;
    using OriginalSolver =
        ConversionOperatingResult (*)(const std::vector<ConversionDevice> &,
                                      const std::vector<ConversionPortConnection> &,
                                      const std::vector<ConversionNonlinearDevice> &,
                                      const std::vector<Complex> &,
                                      const ConversionOperatingOptions &,
                                      bool,
                                      const ConversionNoise *,
                                      const std::vector<Complex> &);
    static_assert(std::is_same<decltype(legacy), OriginalSolver>::value, "legacy function type");
    static_assert(std::is_same<decltype(power), OriginalSolver>::value, "power-wave function type");
    FrequencyConversionModel model(1., {{0, 0}}, {1, {0.}}, {1, {0.}});
    const std::vector<ConversionDevice> devices{
        {model, {1.}, {0.}, model.zero_noise(), model.zero_noise()}};
    int calls = 0;
    const std::vector<ConversionIntrinsicNoiseProvider> providers{
        {0, [&](const ConversionAcceptedState &state) {
             ++calls;
             auto noise = state.jacobian.zero_noise();
             noise.covariance(0, 0) = noise.complementary(0, 0) = 1e-20;
             return ConversionIntrinsicNoiseSnapshot{state.jacobian.spacing_hz(),
                                                     state.jacobian.channels(),
                                                     state.jacobian.reference_impedances_ohms(),
                                                     noise};
         }}};
    const auto first =
        solve_conversion_operating_point_with_intrinsic_noise(devices, {}, {}, providers);
    const auto second = solve_conversion_operating_point_power_waves_with_intrinsic_noise(
        devices, {}, {}, providers);
    return calls == 2 && first.iterations == 0 && second.iterations == 0 &&
           first.waves.outgoing_noise.covariance(0, 0) == Complex{1e-20};
}
