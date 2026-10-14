#include <rfmodel/nonlinear/phd_coefficient_slice.hpp>
#include <cmath>

// Reinstantiate construction, phase freezing, and checked evaluation in a
// second translation unit. The returned value must own the expired source.
bool phd_coefficient_slice_odr_probe() {
    using namespace rfmodel;
    const auto frozen = [] {
        PhdCoefficientData data;
        data.fundamental_hz = 1e9;
        data.port_reference_ohms = {50.};
        data.port_reference_planes = {"probe"};
        data.channels = {{0, 3}, {0, 1}}; // Pump need not be first.
        data.pump_channel = 1;
        data.pump_magnitude_sqrt_w = .7;
        data.baseline_outgoing = {{.2, -.1}, {.4, .3}};
        data.direct = {2, std::vector<Complex>(4)};
        data.conjugate = {2, std::vector<Complex>(4)};
        data.conjugate(0, 0) = {.3, -.2};
        const PhdCoefficientSlice temporary(std::move(data));
        return temporary.freeze_pump(0.);
    }();
    const Complex delta{.02, .03};
    const auto output = frozen.evaluate_incident({delta, .7});
    const auto derivative = frozen.perturbation_jacobian().apply({delta, 0.});
    const auto expected = Complex{.3, -.2} * std::conj(delta);
    return frozen.perturbation_jacobian().pump_channel() == 1 &&
           frozen.coefficients().port_reference_planes[0] == "probe" &&
           std::abs(output[0] - Complex{.2, -.1} - expected) < 1e-14 &&
           std::abs(derivative[0] - expected) < 1e-14 &&
           std::abs(output[1] - Complex{.4, .3}) < 1e-14;
}
