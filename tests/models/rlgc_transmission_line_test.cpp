#include "rfmodel/models/rlgc_transmission_line.hpp"
#include "rfmodel/models/transmission_line.hpp"
#include "rfmodel/linear/network.hpp"
#include "../support/test_support.hpp"
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
    // These expectations use actual binary64 inputs in an independent 100-digit
    // ABCD oracle. Relative checks must not silently accept zero transmission.
    const auto relative_near = [](Complex actual, Complex expected) {
        require(std::abs(expected) > 0., "relative check needs nonzero expected value");
        require(std::abs(actual / expected - Complex{1.}) < 2e-13,
                "relative RLGC boundary mismatch");
    };
    const double maximum = std::numeric_limits<double>::max();
    for (double value : {maximum / 16., maximum / 8.,
                         std::nextafter(maximum / 8., maximum), 1e308, maximum}) {
        const auto series = RlgcTransmissionLineModel("large R", {value, 0., 0., 0.}, 1., 1.)
                                .s_parameters(0.);
        const auto shunt = RlgcTransmissionLineModel("large G", {0., 0., value, 0.}, 1., 1.)
                               .s_parameters(0.);
        near(series(0, 0), 1.);
        near(shunt(0, 0), -1.);
        relative_near(series(1, 0), 2. / value);
        relative_near(shunt(1, 0), 2. / value);
        const auto matched = RlgcTransmissionLineModel("large matched", {value, 0., value, 0.},
                                                      1., 1.)
                                 .s_parameters(0.);
        near(matched(0, 0), 0.);
        near(matched(1, 0), 0.);
    }
    const auto large_inductance =
        RlgcTransmissionLineModel("balanced units L", {0., 1e308, 0., 0.}, 1e-308, 1.)
            .s_parameters(1.);
    near(large_inductance(0, 0), {0.90800033164962475423, 0.28902548222223626121});
    near(large_inductance(1, 0), {0.09199966835037524577, -0.28902548222223626121});
    const auto large_capacitance =
        RlgcTransmissionLineModel("balanced units C", {0., 0., 0., 1e308}, 1e-308, 1.)
            .s_parameters(1.);
    near(large_capacitance(0, 0), -large_inductance(0, 0));
    near(large_capacitance(1, 0), large_inductance(1, 0));
    const auto overflowing_ratio =
        RlgcTransmissionLineModel("balanced ratio", {1e-308, 0., 0., 0.}, 1e308, 1e-308)
            .s_parameters(0.);
    near(overflowing_ratio(0, 0), 1.);
    relative_near(overflowing_ratio(1, 0), 2e-308);
    const auto large_conductance =
        RlgcTransmissionLineModel("balanced units G", {0., 0., 1e160, 0.}, 1e160, 1e-320)
            .s_parameters(0.);
    near(large_conductance(0, 0), -0.33333085936474881289);
    near(large_conductance(1, 0), 0.66666914063525118711);
    const auto zero_extreme = RlgcTransmissionLineModel("zero extreme", {}, 1e308, 1e-308)
                                  .s_parameters(maximum);
    near(zero_extreme(0, 0), 0.);
    near(zero_extreme(1, 0), 1.);
    const auto zero_frequency =
        RlgcTransmissionLineModel("zero frequency", {0., maximum, 0., maximum}, maximum, maximum)
            .s_parameters(0.);
    near(zero_frequency(0, 0), 0.);
    near(zero_frequency(1, 0), 1.);
    const auto tiny_shunt =
        RlgcTransmissionLineModel("underflow recovery", {0., 0., 1e-308, 0.}, 1e-308, 1e308)
            .s_parameters(0.);
    relative_near(tiny_shunt(0, 0), -5e-309);
    near(tiny_shunt(1, 0), 1.);
    const double smallest = std::numeric_limits<double>::denorm_min();
    const auto smallest_series =
        RlgcTransmissionLineModel("smallest reflection", {2. * smallest, 0., 0., 0.}, 1., 1.)
            .s_parameters(0.);
    require(smallest_series(0, 0) == Complex{smallest}, "lost smallest reflection");
    constexpr double two_pi = 6.28318530717958647692;
    const double large_component = 1.7e308;
    const double large_reactive = large_component / two_pi;
    // a=b=x has finite components, although the unscaled sqrt(a)*sqrt(b)
    // materializes an overflowing real product before its subtraction.
    const auto large_complex =
        RlgcTransmissionLineModel("large complex matched",
                                  {large_component,
                                   large_reactive,
                                   large_component,
                                   large_reactive},
                                  1.,
                                  1.)
            .s_parameters(1.);
    near(large_complex(0, 0), 0.);
    near(large_complex(1, 0), 0.);
    for (double length : {1e-300, 1., 1e300}) {
        // Unit rescaling must preserve the normalized network and its dual.
        const auto original =
            RlgcTransmissionLineModel("units", {3. / length, 2. / length, 5. / length, 7. / length},
                                      length, 1.)
                .s_parameters(0.1);
        const auto dual =
            RlgcTransmissionLineModel("dual", {5. / length, 7. / length, 3. / length, 2. / length},
                                      length, 1.)
                .s_parameters(0.1);
        const auto base = RlgcTransmissionLineModel("base", {3., 2., 5., 7.}, 1., 1.)
                              .s_parameters(0.1);
        near(original(0, 0), base(0, 0));
        near(original(1, 0), base(1, 0));
        near(dual(0, 0), -original(0, 0));
        near(dual(1, 0), original(1, 0));
        near(original(0, 1), original(1, 0));
        near(original(1, 1), original(0, 0));
        passive_thermal_noise(original, 290.);
    }
    rejects<std::overflow_error>([&] {
        RlgcTransmissionLineModel("series overflow", {maximum, 0., 0., 0.}, 2., 1.)
            .s_parameters(0.);
    });
    rejects<std::overflow_error>([&] {
        RlgcTransmissionLineModel("shunt overflow", {0., 0., maximum, 0.}, 2., 1.)
            .s_parameters(0.);
    });
    rejects<std::overflow_error>([&] {
        RlgcTransmissionLineModel("reactive overflow", {0., maximum, 0., 0.}, 1., 1.)
            .s_parameters(maximum);
    });
    rejects<std::overflow_error>([&] {
        // Finite a,b, but Im(sqrt(a)*sqrt(b)) exceeds DBL_MAX.
        RlgcTransmissionLineModel("electrical length overflow",
                                  {0., large_reactive, large_component, large_reactive}, 1., 1.)
            .s_parameters(1.);
    });
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
