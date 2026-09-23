#include "rfmodel/power_gain.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const SMatrix matched{2, {0., 0., 2., 0.}};
    near(transducer_power_gain(matched), 4.);
    near(operating_power_gain(matched), 4.);
    near(available_power_gain(matched), 4.);
    const SMatrix mismatched{2, {1. / 3., 0., 2., 0.5}};
    near(transducer_power_gain(mismatched), 4.);
    near(operating_power_gain(mismatched), 4.5);
    near(available_power_gain(mismatched), 16. / 3.);

    const SMatrix bilateral{2, {{0.2, 0.1}, {0.05, -0.02}, {2., 0.3}, {-0.1, 0.2}}};
    for (const Complex source : {Complex{}, Complex{0.2, 0.15}, Complex{-0.3, 0.1}}) {
        for (const Complex load : {Complex{}, Complex{0.1, -0.25}, Complex{-0.2, 0.1}}) {
            LinearNetwork network;
            network.add(bilateral);
            network.terminate(0, source, 1.);
            network.terminate(1, load);
            const auto waves = network.solve();
            const double delivered = std::norm(waves.outgoing[1]) - std::norm(waves.incident[1]);
            const double accepted = std::norm(waves.incident[0]) - std::norm(waves.outgoing[0]);
            const double source_available = 1. / (1. - std::norm(source));
            near(transducer_power_gain(bilateral, source, load), delivered / source_available);
            near(operating_power_gain(bilateral, load), delivered / accepted);
        }
        // Available gain equals delivered gain at the conjugate output match.
        const Complex output = bilateral(1, 1) + bilateral(0, 1) * bilateral(1, 0) * source /
                                                     (1. - bilateral(0, 0) * source);
        near(available_power_gain(bilateral, source),
             transducer_power_gain(bilateral, source, std::conj(output)));
    }
    near(transducer_power_gain(matched, {}, 1.), 0.);
    near(operating_power_gain(matched, -1.), 0.);
    near(available_power_gain(SMatrix{2, {0., 0., 0., 0.}}), 0.);
    // Direct determinant remains defined even when 1-S22*Gamma_load is zero.
    const SMatrix active_output{2, {0., 1., 1., 2.}};
    near(transducer_power_gain(active_output, 0.2, 0.5), 72.);
    rejects<std::domain_error>([&] {
        available_power_gain(active_output);
    });
    rejects<std::domain_error>([&] {
        operating_power_gain(SMatrix{2, {1., 0., 2., 0.}});
    });
    rejects<std::domain_error>([&] {
        transducer_power_gain(SMatrix{2, {2., 0., 1., 0.}}, 0.5);
    });
    rejects<std::invalid_argument>([&] {
        transducer_power_gain(matched, 1.);
    });
    rejects<std::invalid_argument>([&] {
        operating_power_gain(matched, {std::numeric_limits<double>::quiet_NaN(), 0});
    });
    rejects<std::invalid_argument>([] {
        available_power_gain(SMatrix{2, {0.}});
    });
    rejects<std::overflow_error>([] {
        transducer_power_gain(SMatrix{2, {0., 0., 1e200, 0.}});
    });
}
