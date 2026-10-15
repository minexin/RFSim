#include <rfmodel/conversion/tabulated_amplifier_linearization.hpp>
#include <rfmodel/adapters/builtin_model_adapters.hpp>

rfmodel::ModelAdapter owned_table_adapter() {
    return rfmodel::adapt_tabulated_amplifier(
        "owned table", rfmodel::TabulatedAmplifier({{0., 0., .3, 2., 0.}, {1., 2., .3, 2., 0.}}));
}

double second_translation_unit() {
    const rfmodel::TabulatedAmplifier amp({{0., 0., 0., 2., 0.}, {1., 2., 0., 2., 0.}});
    return amp.response(0.).gain.real();
}
