#include "rfmodel/transmission_line.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    TransmissionLineModel line("100 ohm line", 100., 0.25e-9);
    const auto quarter = line.s_parameters(1e9);
    near(quarter(0, 0), 0.6);
    near(quarter(1, 0), Complex{0., -0.8});
    near(line.s_parameters(2e9)(1, 0), -1.);
    near(line.s_parameters(2e9)(0, 0), 0.);
    near(line.s_parameters(0.)(1, 0), 1.);
    near(line.port(1).reference_impedance, 50.);
    for (double frequency : {0., 1e6, 1e9, 1.3e9, 2e9}) {
        const auto s = line.s_parameters(frequency);
        near(std::norm(s(0, 0)) + std::norm(s(1, 0)), 1.);
        near(s(0, 0) * std::conj(s(0, 1)) + s(1, 0) * std::conj(s(1, 1)), 0.);
        TransmissionLineModel matched("matched", 75., 0.25e-9, 3., 75.);
        MatchedTransmissionModel existing("existing", 3., 0.25e-9, 75.);
        near(matched.s_parameters(frequency)(1, 0), existing.s_parameters(frequency)(1, 0));
        near(matched.s_parameters(frequency)(0, 0), 0.);
        // Two uniform sections must equal a single section of double length/loss.
        TransmissionLineModel lossy("section", 100., 0.25e-9, 2.);
        TransmissionLineModel doubled("double", 100., 0.5e-9, 4.);
        LinearNetwork network;
        network.add(lossy.s_parameters(frequency));
        network.add(lossy.s_parameters(frequency));
        network.connect(1, 2);
        const auto combined = network.external_s({0, 3});
        const auto expected = doubled.s_parameters(frequency);
        for (std::size_t i = 0; i < 2; ++i) {
            for (std::size_t j = 0; j < 2; ++j) {
                near(combined(i, j), expected(i, j));
            }
        }
    }
    TransmissionLineModel opaque("opaque", 100., 1., 10000.);
    near(opaque.s_parameters(1.)(0, 0), 1. / 3.);
    near(opaque.s_parameters(1.)(1, 0), 0.);
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 0., 0.);
    });
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 100., -1.);
    });
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 100., 0., -1.);
    });
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 1e300, 0.);
    });
    rejects<std::invalid_argument>([&] {
        line.s_parameters(-1.);
    });
    rejects<std::invalid_argument>([&] {
        line.s_parameters(std::numeric_limits<double>::infinity());
    });
    rejects<std::out_of_range>([&] {
        line.port(2);
    });
}
