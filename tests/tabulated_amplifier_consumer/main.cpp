#include <rfmodel/conversion/tabulated_amplifier_linearization.hpp>
#include <cmath>
extern double second_translation_unit();

int main() {
    rfmodel::TabulatedAmplifier amp({{0., 0., 0., 2., 0.}, {1., 2., 0., 2., 0.}});
    auto law = rfmodel::make_tabulated_amplifier_device(0, 1e6, {{0, 1}, {1, 1}}, amp);
    auto p = law.evaluate({.25, 0.});
    return std::abs(p.outgoing[1] - .5) < 1e-12 && second_translation_unit() == 2. ? 0 : 1;
}
