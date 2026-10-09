#include "rfmodel/coherent_amplifier.hpp"
#include "rfmodel/coherent_polynomial.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    for (double reference : {50., 75.}) {
        const CoherentLimitedAmplifier amplifier(20., 20., 23., 20., 10., reference);
        const SaturatingFundamentalCompression fundamental(20., 20., 23.);
        const auto zero = amplifier.operating_point(0.);
        near(zero.fundamental_amplitude_gain, 10.);
        near(zero.nonlinear_input_scale, 1.);
        near(zero.limited_input_power_w, 0.);
        for (double drive : {1e-12, .0001, fundamental.input_p1db_watts(), .01, 1., 1e300}) {
            const std::vector<CoherentComponent> input{
                {10, SpectrumKind::source, 1., 7, std::sqrt(drive / 5.)},
                {11, SpectrumKind::source, 1., 9, Complex(0., std::sqrt(drive * .8))}};
            const auto expected = amplifier.evaluate(1e8, input);
            const auto point = amplifier.operating_point(expected.total_input_power_w);
            near(point.limited_input_power_w, expected.limited_input_power_w);
            require(point.fundamental_amplitude_gain > 0. && point.nonlinear_input_scale > 0. &&
                        point.nonlinear_input_scale <= 1.,
                    "valid common response");
            auto scaled = expected.inputs;
            for (auto &component : scaled) {
                component.amplitude *= point.nonlinear_input_scale;
            }
            const auto nonlinear =
                CoherentPolynomial(
                    {0., 0., point.quadratic_voltage_coefficient, point.cubic_voltage_coefficient},
                    reference)
                    .evaluate(1e8, scaled);
            for (const auto &term : expected.terms) {
                if (term.order == 1) {
                    const auto incident = expected.inputs[term.input_indices[0] - 1].amplitude;
                    near(incident * point.fundamental_amplitude_gain, term.component.amplitude);
                } else {
                    auto found = std::find_if(
                        nonlinear.terms.begin(), nonlinear.terms.end(), [&](const auto &candidate) {
                            return candidate.order == term.order &&
                                   std::equal(term.input_indices.begin(),
                                              term.input_indices.begin() + term.order,
                                              candidate.input_indices.begin());
                        });
                    require(found != nonlinear.terms.end(), "nonlinear path retained");
                    require(std::abs(found->component.amplitude - term.component.amplitude) <=
                                1e-15 + 1e-11 * std::abs(term.component.amplitude),
                            "working point reproduces independent legacy path");
                }
            }
        }
        for (double bad : {-1.,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            rejects<std::invalid_argument>([&] {
                amplifier.operating_point(bad);
            });
            rejects<std::invalid_argument>([&] {
                fundamental.amplitude_gain(bad);
            });
        }
        // Opposing contributions can each exceed physical drive, including drive=0.
        const Complex a{.3, -.4};
        near(a * zero.fundamental_amplitude_gain + (-a) * zero.fundamental_amplitude_gain, {});
    }
    const P1dBFundamentalCompression low(20., 20.);
    near(low.amplitude_gain(0.), 10.);
    near(low.amplitude_gain(low.input_p1db_watts()), 10. * std::pow(10., -.05));
    rejects<std::out_of_range>([&] {
        low.amplitude_gain(2. * low.input_p1db_watts());
    });
}
