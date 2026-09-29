#include "rfmodel/rlgc_transmission_line.hpp"
#include "rfmodel/transmission_line.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    // sqrt(L/C)=100 ohm; length*sqrt(L*C)=2 ns.
    RlgcTransmissionLineModel lossless("LC", {0., 1e-6, 0., 1e-10}, 0.2);
    TransmissionLineModel analytic("ideal", 100., 2e-9);
    for (double f : {0., 1e-6, 1e3, 125e6, 250e6, 1.3e9}) {
        const auto actual = lossless.s_parameters(f);
        const auto expected = analytic.s_parameters(f);
        near(actual(0, 0), expected(0, 0));
        near(actual(1, 0), expected(1, 0));
        passive_thermal_noise(actual, 290.);
    }
    // DC degeneracies: no division by zero characteristic impedance.
    RlgcTransmissionLineModel resistor("R", {25., 1e-6, 0., 1e-10}, 2.);
    near(resistor.s_parameters(0.)(0, 0), 1. / 3.);
    near(resistor.s_parameters(0.)(1, 0), 2. / 3.);
    RlgcTransmissionLineModel conductance("G", {0., 0., 0.01, 0.}, 2.);
    near(conductance.s_parameters(0.)(0, 0), -1. / 3.);
    near(conductance.s_parameters(0.)(1, 0), 2. / 3.);
    RlgcTransmissionLineModel empty("zero", {}, 10.);
    near(empty.s_parameters(1e9)(1, 0), 1.);
    RlgcTransmissionLineModel no_length("zero length", {1., 1., 1., 1.}, 0.);
    near(no_length.s_parameters(1e300)(1, 0), 1.);
    // Distortionless line: R/L=G/C, Zc=50 and gamma=0.02+j*omega*5e-9 per metre.
    RlgcTransmissionLineModel balanced("balanced", {1., 250e-9, 0.0004, 100e-12}, 3.);
    for (double f : {0., 1e6, 1e9}) {
        const auto matrix = balanced.s_parameters(f);
        near(matrix(0, 0), 0.);
        near(matrix(1, 0), std::polar(std::exp(-0.06), -6.28318530717958647692 * f * 15e-9));
    }
    // General lossy line: two exact sections equal one double-length section.
    RlgcPerLength parameters{7., 400e-9, 0.002, 120e-12};
    RlgcTransmissionLineModel section("section", parameters, 0.7);
    RlgcTransmissionLineModel doubled("double", parameters, 1.4);
    for (double f : {0., 1e-6, 1e6, 3e9}) {
        LinearNetwork network;
        network.add(section.s_parameters(f));
        network.add(section.s_parameters(f));
        network.connect(1, 2);
        const auto combined = network.external_s({0, 3});
        const auto expected = doubled.s_parameters(f);
        near(combined(0, 0), expected(0, 0));
        near(combined(1, 0), expected(1, 0));
        passive_thermal_noise(expected, 290.);
    }
    RlgcTransmissionLineModel long_line("long", {1., 250e-9, 0.0004, 100e-12}, 1e6);
    near(long_line.s_parameters(1e9)(1, 0), 0.);
    near(long_line.s_parameters(1e9)(0, 0), 0.);
    near(section.port(1).reference_impedance, 50.);
    rejects<std::invalid_argument>([] {
        RlgcTransmissionLineModel bad("bad", {-1., 0., 0., 0.}, 1.);
    });
    rejects<std::invalid_argument>([] {
        RlgcTransmissionLineModel bad("bad", {}, -1.);
    });
    rejects<std::invalid_argument>([] {
        RlgcTransmissionLineModel bad("bad", {}, 1., 0.);
    });
    rejects<std::invalid_argument>([&] {
        section.s_parameters(-1.);
    });
    rejects<std::invalid_argument>([&] {
        section.s_parameters(std::numeric_limits<double>::infinity());
    });
    rejects<std::out_of_range>([&] {
        section.port(2);
    });
}
