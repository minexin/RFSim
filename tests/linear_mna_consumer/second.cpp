#include <rfmodel/mna/linear_network.hpp>
#include <cmath>

bool second_translation_unit() {
    using namespace rfmodel::mna;
    NetworkBuilder b;
    auto p = b.add_node();
    b.add_current_source(ground, p, {2, {0, 1}});
    b.add_resistor(p, ground, 3);
    return std::abs(solve_dc(b.finalize()).node_voltage[p.value] - 6) < 1e-12;
}
