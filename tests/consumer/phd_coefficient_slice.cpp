// Standalone C++17 consumer: public installed header only, with a second TU.
#include <rfmodel/nonlinear/phd_coefficient_slice.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>

bool phd_coefficient_slice_odr_probe();

namespace {
using namespace rfmodel;

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void close(Complex actual, Complex expected, const char *message) {
    require(std::isfinite(std::abs(actual - expected)) && std::abs(actual - expected) < 1e-12,
            message);
}

void installed_clamped_slice() {
    static_assert(!std::is_convertible<PhdClampedJacobian, FrequencyConversionModel>::value,
                  "clamped Jacobian cannot become an unrestricted conversion model");
    PhdCoefficientData data;
    data.fundamental_hz = 2.4e9;
    data.port_reference_ohms = {50., 75.};
    data.port_reference_planes = {"source:connector", "device:load"};
    data.channels = {{0, 1}, {1, 0}, {1, 2}};
    data.pump_channel = 0;
    data.pump_magnitude_sqrt_w = .5;
    data.baseline_outgoing = {{.2, -.1}, .3, {.4, .2}};
    data.direct = {3, std::vector<Complex>(9)};
    data.conjugate = {3, std::vector<Complex>(9)};
    data.direct(1, 1) = data.conjugate(1, 1) = .2;
    data.direct(1, 2) = {.1, -.2};
    data.conjugate(1, 2) = {.1, .2};
    data.direct(2, 1) = data.conjugate(2, 1) = {.05, .02};
    data.direct(2, 2) = {.3, .1};
    data.conjugate(2, 2) = {-.07, .04};
    data.provenance = "installed native coefficient fixture";
    const PhdCoefficientSlice slice(data);
    data.baseline_outgoing.assign(3, 100.); // Caller storage is no longer shared.
    const double phase = std::acos(-1.) / 2.;
    const auto frozen = slice.freeze_pump(phase);
    const auto &jacobian = frozen.perturbation_jacobian();
    close(frozen.nominal_incident()[0], {0., .5}, "installed pump phase");
    close(frozen.nominal_outgoing()[1], .3, "installed real DC baseline");
    close(frozen.nominal_outgoing()[2], {-.4, -.2}, "installed second-harmonic phase");
    close(jacobian.direct(1, 2), {-.1, .2}, "installed negative phase exponent");
    close(jacobian.conjugate(1, 2), {-.1, -.2}, "installed positive phase exponent");
    close(jacobian.direct(2, 2), {.3, .1}, "installed direct identity exponent");
    close(jacobian.conjugate(2, 2), {-.07, .04}, "installed conjugate fourth-harmonic exponent");
    const std::vector<Complex> delta{0., .02, {.03, -.04}};
    const auto variation = jacobian.apply(delta);
    const auto output = frozen.evaluate_perturbation(delta);
    auto total = frozen.nominal_incident();
    for (std::size_t i = 0; i < total.size(); ++i) {
        total[i] += delta[i];
    }
    const auto absolute_output = frozen.evaluate_incident(total);
    for (std::size_t i = 0; i < total.size(); ++i) {
        close(output[i],
              frozen.output_offset()[i] + variation[i],
              "installed affine offset identity");
        close(output[i], absolute_output[i], "installed absolute and delta evaluators agree");
    }
    require(output[1].imag() == 0., "installed DC output stays exactly real");
    close(output[1], .318, "installed hand-computed DC response");
    close(output[2], {-.3927, -.2114}, "installed hand-computed complex response");
    require(frozen.coefficients().port_reference_ohms == std::vector<double>({50., 75.}) &&
                frozen.coefficients().provenance == "installed native coefficient fixture",
            "installed native metadata survives ownership");
    bool rejected = false;
    try {
        (void)jacobian.apply({1e-30, 0., 0.});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "installed Jacobian rejects even tiny pump changes");
}
} // namespace

int main() {
    try {
        installed_clamped_slice();
        require(phd_coefficient_slice_odr_probe(), "second translation unit and ownership probe");
        std::cout << "Installed native pump-clamped PHD slice consumer passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
