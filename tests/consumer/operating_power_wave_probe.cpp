#include <rfmodel/conversion/conversion_operating_point.hpp>
#include <cmath>
#include <vector>

// Instantiate the public inline entry in a second translation unit. Using only
// its three required arguments also checks that the installed defaults exist.
bool operating_power_wave_odr_probe() {
    using namespace rfmodel;
    const std::vector<ConversionChannel> channels{{0, 0}, {0, 1}};
    const std::vector<Complex> references{{75., 0.}, {31., 12.}};
    const SMatrix zero{2, std::vector<Complex>(4)};
    const FrequencyConversionModel placeholder(1e8, channels, zero, zero, references);
    const std::vector<Complex> source{.2, {.15, -.1}};
    const ConversionDevice device{
        placeholder, source, {0., 0.}, placeholder.zero_noise(), placeholder.zero_noise()};
    const ConversionNonlinearDevice law{
        0, [channels, references](const std::vector<Complex> &a) {
            SMatrix direct{2, std::vector<Complex>(4)}, conjugate{2, std::vector<Complex>(4)};
            direct(0, 0) = 2. * a[0];
            direct(1, 1) = -std::norm(a[1]);
            conjugate(1, 1) = -.5 * a[1] * a[1];
            return ConversionLinearization{
                FrequencyConversionModel(1e8, channels, direct, conjugate, references),
                {a[0] * a[0], -.5 * std::norm(a[1]) * a[1]}};
        }};
    const auto result = solve_conversion_operating_point_power_waves({device}, {}, {law});
    return result.iterations == 0 && result.scaled_residual == 0. &&
           result.waves.incident == source &&
           result.linearized_devices[0].model.reference_impedances_ohms() == references &&
           result.waves.outgoing[0].imag() == 0. &&
           std::abs(result.waves.outgoing[0] - Complex{.04, 0.}) < 1e-14 &&
           std::abs(result.waves.outgoing[1] + .5 * std::norm(source[1]) * source[1]) < 1e-14;
}
