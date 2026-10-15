#include <rfmodel/power_wave_s.hpp>
#include <cmath>

bool second_translation_unit() {
    using namespace rfmodel::mna;
    const auto block =
        extract_power_wave_s(NetworkBuilder{}.finalize(), 1, {{ground, ground}}, {{4, 3}});
    const auto value = block.scattering(0, 0);
    return std::isfinite(value.real()) && std::isfinite(value.imag()) &&
           std::abs(value - Complex{-7, 24} / 25.0) < 1e-12 &&
           block.references_ohm == std::vector<Complex>{{4, 3}};
}
