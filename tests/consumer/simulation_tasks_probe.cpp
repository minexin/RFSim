#include <rfmodel/analysis/analysis_task.hpp>
#include <rfmodel/coherent/coherent_system.hpp>
#include <rfmodel/conversion/conversion_simulation.hpp>

// A second translation unit checks header-only linkage and owning plan lifetime.
rfmodel::CoherentSystem::Plan make_simulation_plan() {
    rfmodel::CoherentSystem graph(1e8, 50., {{"rf", "clock"}, {"lo", "clock"}});
    auto input = graph.input("rf", {{5, rfmodel::SpectrumKind::source, 1., 1, .02}});
    graph.mixer_bank("mixer", {{"if", input, {7, 1, 0., 0.}}});
    return graph.compile();
}
