#include "rfmodel/linear_analysis.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    auto build = [](double f) {
        LinearNetwork network(75.);
        network.add(SMatrix{2, {0., 0.25, f / 1e9, 0.}}, 75.);
        return network;
    };
    auto result = analyze_linear(FrequencyGrid{{1e9, 2e9}}, {0, 1}, build);
    require(result.scattering.size() == 2, "frequency dimension");
    require(result.port_impedances_ohms[1].size() == 2, "port dimension");
    near(result.scattering[0](1, 0), 1.);
    near(result.scattering[1](1, 0), 2.);
    near(result.scattering[1](0, 1), 0.25);
    near(result.port_impedances_ohms[1][0], 75.);
    auto reversed = analyze_linear(FrequencyGrid{{1e9}}, {1, 0}, build);
    near(reversed.scattering[0](0, 1), 1.);
    rejects<std::invalid_argument>([&] {
        analyze_linear(FrequencyGrid{{2., 1.}}, {0, 1}, build);
    });
    rejects<std::runtime_error>([&] {
        analyze_linear(FrequencyGrid{{1e9}}, {0, 0}, build);
    });
}
