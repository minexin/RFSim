#include <rfmodel/conversion/tabulated_amplifier_linearization.hpp>
#include <rfmodel/adapters/builtin_model_adapters.hpp>
#include <cmath>
extern double second_translation_unit();
rfmodel::ModelAdapter owned_table_adapter();

int main() {
    rfmodel::TabulatedAmplifier amp({{0., 0., 0., 2., 0.}, {1., 2., 0., 2., 0.}});
    auto law = rfmodel::make_tabulated_amplifier_device(0, 1e6, {{0, 1}, {1, 1}}, amp);
    auto p = law.evaluate({.25, 0.});
    const auto adapter = owned_table_adapter();
    const auto expected = std::polar(.5, .3);
    const auto spectrum = adapter.transmit({1e8, {{1, .25}}});
    if (std::abs(spectrum.amplitudes.at(1) - expected) > 1e-12) {
        return 2;
    }
    rfmodel::CoherentSystem graph(1e8, 50., {{"rf", "clock"}});
    auto input = graph.input("in", {{1, rfmodel::SpectrumKind::source, 1., 1, .25}});
    adapter.coherent_stage(graph, "table", {input}, {"out"});
    const auto result = graph.compile().execute();
    if (std::abs(result.stream("out").reduction.components.at(0).amplitude - expected) > 1e-12) {
        return 3;
    }
    return std::abs(p.outgoing[1] - .5) < 1e-12 && second_translation_unit() == 2. ? 0 : 1;
}
