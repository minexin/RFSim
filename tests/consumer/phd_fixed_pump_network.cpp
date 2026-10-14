// Installed public header, deliberately linked with a second independent TU.
#include <rfmodel/nonlinear/phd_fixed_pump_network.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>

bool phd_fixed_pump_network_odr_probe();

int main() {
    try {
        using namespace rfmodel;
        static_assert(!std::is_convertible<PhdFixedPumpNetwork, FrequencyConversionModel>::value,
                      "restricted network must not expose an unrestricted model");
        PhdCoefficientData d;
        d.fundamental_hz = 1e9;
        d.port_reference_ohms = {50.};
        d.port_reference_planes = {"installed"};
        d.channels = {{0, 1}, {0, 2}};
        d.pump_channel = 0;
        d.pump_magnitude_sqrt_w = .8;
        d.baseline_outgoing = {{.2, .1}, {.4, -.3}};
        d.direct = {2, std::vector<Complex>(4)};
        d.conjugate = {2, std::vector<Complex>(4)};
        d.direct(1, 1) = {.23, .11};
        d.conjugate(1, 1) = {-.17, .09};
        const auto f = PhdCoefficientSlice(d).freeze_pump(0.);
        const Complex gamma{.31, .17}, source{.03, -.04};
        PhdFixedPumpNetwork network(f, {{{0, 2}, source, gamma}, {{0, 1}, .8, 0.}}, {{{0, 2}, .5}});
        const auto r = network.solve();
        const Complex alpha = 1. - gamma * d.direct(1, 1), beta = -gamma * d.conjugate(1, 1);
        const Complex rhs = source + gamma * d.baseline_outgoing[1];
        const double xx = (alpha + beta).real(), xy = -(alpha - beta).imag();
        const double yx = (alpha + beta).imag(), yy = (alpha - beta).real(),
                     det = xx * yy - xy * yx;
        const Complex expected{(rhs.real() * yy - xy * rhs.imag()) / det,
                               (xx * rhs.imag() - rhs.real() * yx) / det};
        if (!r.pump_clamp_exact || r.solved_pump != Complex{.8, 0.} ||
            std::abs(r.channels[1].incident - expected) > 1e-12 || r.relative_residual > 1e-12 ||
            !phd_fixed_pump_network_odr_probe()) {
            throw std::runtime_error("installed fixed-pump network contract failed");
        }
        std::cout << "Installed fixed-pump PHD network two-TU consumer passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
