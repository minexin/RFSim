#include "rfmodel/source_coherence.hpp"
#include "rfmodel/coherence.hpp"
#include "test_support.hpp"
#include <algorithm>

int main() {
    using namespace rfmodel;
    std::vector<SourceCoherence> definitions{
        {"a", "clock"}, {"b", "clock"}, {"clock", ""}, {"c", ""}};
    const auto ids = assign_source_coherence(definitions);
    require(ids[0] == ids[1], "shared clock must be coherent");
    require(ids[0] != ids[2], "clock and source namespaces must differ");
    require(ids[2] != ids[3], "independent sources must differ");
    std::reverse(definitions.begin(), definitions.end());
    auto reversed = assign_source_coherence(definitions);
    std::reverse(reversed.begin(), reversed.end());
    require(ids == reversed, "input order must not change assignment");
    const auto distinct = assign_source_coherence({{"a", "Clock"}, {"b", "clock"}});
    require(distinct[0] != distinct[1], "labels are case sensitive");
    CoherentComponent first{10, SpectrumKind::source, 1., ids[0], 1.};
    auto second = first;
    second.coherence_group = ids[1];
    second.amplitude = -1.;
    near(reduce_coherent_components(1e8, {first, second}).total_power_w, 0.);
    second.bandwidth_hz = 2.;
    near(reduce_coherent_components(1e8, {first, second}).total_power_w, 2.);
    second.bandwidth_hz = 1.;
    second.kind = SpectrumKind::intermod;
    near(reduce_coherent_components(1e8, {first, second}).total_power_w, 2.);
    second.kind = SpectrumKind::source;
    second.bin = 11;
    near(reduce_coherent_components(1e8, {first, second}).total_power_w, 2.);
    require(assign_source_coherence({}).empty(), "empty source set");
    for (const auto &bad :
         std::vector<std::vector<SourceCoherence>>{{{"", ""}},
                                                   {{"a", ""}, {"a", ""}},
                                                   {{"a", "c"}, {"a", "d"}},
                                                   {{std::string(1025, 'a'), ""}},
                                                   {{"a", std::string(1025, 'c')}},
                                                   {{std::string("a\0b", 3), ""}}}) {
        rejects<std::invalid_argument>([&] {
            assign_source_coherence(bad);
        });
    }
    rejects<std::invalid_argument>([] {
        assign_source_coherence(std::vector<SourceCoherence>(4097, {"a", ""}));
    });
}
