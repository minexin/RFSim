#include "rfmodel/network_parameters.hpp"
#include "rfmodel/measurements.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    // Physical 50-ohm load has Gamma=-0.2 with 75-ohm reference.
    auto load = renormalize_s(SMatrix{1, {0.}}, 50., 75.);
    near(load(0, 0), -0.2);
    near(input_impedance(load(0, 0), 75.), 50.);
    near(renormalize_s(SMatrix{1, {1.}}, 50., 75.)(0, 0), 1.);
    near(renormalize_s(SMatrix{1, {-1.}}, 50., 75.)(0, 0), -1.);
    const SMatrix thru{2, {0., 1., 1., 0.}};
    auto changed = renormalize_s(thru, 50., 75.);
    for (std::size_t i = 0; i < 4; ++i) {
        near(changed.values[i], thru.values[i]);
    }
    const SMatrix original{2, {Complex{0.1, 0.2}, Complex{0.2, -0.1}, Complex{0.7, 0.1}, -0.1}};
    auto s75 = renormalize_s(original, 50., 75.);
    auto recovered = renormalize_s(s75, 75., 50.);
    auto z50 = s_to_z(original, 50.), z75 = s_to_z(s75, 75.);
    for (std::size_t i = 0; i < 4; ++i) {
        near(recovered.values[i], original.values[i]);
        near(z50.values[i], z75.values[i]);
    }
    LinearNetwork network;
    network.add(thru);
    const auto p = network.add(renormalize_s(SMatrix{1, {-0.2}}, 75., 50.));
    network.connect(1, p);
    near(network.external_s({0})(0, 0), 0.);
    rejects<std::invalid_argument>([&] {
        renormalize_s(original, 0., 75.);
    });
    rejects<std::domain_error>([] {
        renormalize_s(SMatrix{1, {5.}}, 50., 75.);
    });
}
