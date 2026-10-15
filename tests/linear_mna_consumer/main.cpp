#include <rfmodel/mna/linear_network.hpp>
#include <cmath>

bool second_translation_unit();

int main() {
    using namespace rfmodel::mna;
    NetworkBuilder b;
    const auto a = b.add_node(), c = b.add_node();
    const auto source = b.add_voltage_source(a, ground, {12, {3, 4}});
    b.add_resistor(a, c, 2000);
    b.add_resistor(c, ground, 1000);
    const auto n = b.finalize();
    const auto dc = solve_dc(n);
    const auto ac = solve_ac(n, 1000);
    return std::abs(dc.node_voltage[c.value] - 4) < 1e-12 &&
                   std::abs(dc.elements[source.value].current + .004) < 1e-15 &&
                   std::abs(ac.node_voltage[c.value] - Complex{1, 4.0 / 3}) < 1e-12 &&
                   second_translation_unit()
               ? 0
               : 1;
}
