#include <rfmodel/mna/power_wave_s.hpp>
#include <rfmodel/linear/network.hpp>
#include <cmath>

bool second_translation_unit();

int main() {
    using namespace rfmodel::mna;
    NetworkBuilder b;
    const auto p = b.add_node(), q = b.add_node();
    b.add_voltage_source(p, q, {100, {3, 4}});
    const auto block =
        extract_power_wave_s(b.finalize(), 1, {{p, ground}, {q, ground}}, {{4, 3}, {9, -2}});
    rfmodel::LinearNetwork joined;
    const auto offset = joined.add_power_waves(block.scattering, block.references_ohm);
    joined.terminate(offset + 1, -2.0 / 7);
    const auto result = joined.external_s({offset})(0, 0);
    return std::isfinite(result.real()) && std::isfinite(result.imag()) &&
                   std::abs(result - Complex{5, 4} / 41.0) < 1e-12 &&
                   block.column_diagnostics.size() == 2 && second_translation_unit()
               ? 0
               : 1;
}
