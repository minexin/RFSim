#include <rfmodel/frequency_dependent_cable.hpp>
#include <cmath>
#include <stdexcept>

// Exercise all three public entry points from a second translation unit.
// No assert(): the installed Release build must execute every check.
bool frequency_dependent_cable_odr_probe() {
    using namespace rfmodel;
    FrequencyDependentCableParameters p;
    p.length_m = .75;
    p.characteristic_ohms = 73.;
    p.relative_permittivity = 2.25;
    p.reference_frequency_hz = 4e9;
    p.sqrt_loss_db_per_m = .8;
    p.linear_loss_db_per_m = .2;
    p.reference_ohms = 73.;
    constexpr double frequency = 1e9;
    const double expected_loss = .75 * (.8 * .5 + .2 * .25);
    if (std::abs(cable_propagation_loss_db(frequency, p) - expected_loss) > 1e-14) {
        return false;
    }
    const double delay = .75 * 1.5 / 299792458.;
    const Complex expected =
        std::polar(std::pow(10., -expected_loss / 20.), -2. * std::acos(-1.) * frequency * delay);
    const auto s = frequency_dependent_cable_s_parameters(frequency, p);
    const FrequencyDependentCableModel cable("second translation unit", p);
    const auto model = cable.s_parameters(frequency);
    if (s.ports != 2 || model.values != s.values || s(0, 0) != Complex{} || s(1, 1) != Complex{} ||
        std::abs(s(1, 0) - expected) > 2e-13 || std::abs(s(0, 1) - expected) > 2e-13) {
        return false;
    }
    bool rejected = false;
    p.linear_loss_db_per_m = -1.;
    try {
        cable_propagation_loss_db(frequency, p);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    return rejected;
}
