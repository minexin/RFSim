#include <rfmodel/conversion/tabulated_amplifier_linearization.hpp>

double second_translation_unit() {
    const rfmodel::TabulatedAmplifier amp({{0., 0., 0., 2., 0.}, {1., 2., 0., 2., 0.}});
    return amp.response(0.).gain.real();
}
