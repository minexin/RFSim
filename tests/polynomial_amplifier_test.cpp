#include "rfmodel/polynomial_amplifier.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    MatchedPolynomialAmplifier linear("linear", {0., 2.}, 75.);
    const PowerWaveSpectrum input{1e6, {{0, .2}, {10, Complex{.1, -.2}}}};
    const auto result = linear.transmit(input);
    near(result.amplitudes.at(0), .4);
    near(result.amplitudes.at(10), Complex{.2, -.4});
    near(linear.s_parameters(1e9)(1, 0), 2.);
    near(linear.port(0).reference_impedance, 75.);
    const auto amplifier = MatchedPolynomialAmplifier::from_iip3("cubic", 20., 10.);
    constexpr double intercept = .01;
    const double compression_input = (1. - std::pow(10., -1. / 20.)) * intercept;
    const auto compressed = amplifier.transmit({1e6, {{10, std::sqrt(compression_input)}}});
    const double gain = std::norm(compressed.amplitudes.at(10)) / compression_input;
    near(10. * std::log10(gain), 19.);
    constexpr double tone_power = 1e-5;
    const auto tones =
        amplifier.transmit({1e6, {{10, std::sqrt(tone_power)}, {13, std::sqrt(tone_power)}}});
    const double im3_power = std::norm(tones.amplitudes.at(7));
    near(im3_power / (100. * tone_power * std::pow(tone_power / intercept, 2)), 1.);
    // IP3 mapping must describe the same matched wave behavior at 75 ohms.
    const auto alternate = MatchedPolynomialAmplifier::from_iip3("75", 20., 10., 75.);
    const auto other =
        alternate.transmit({1e6, {{10, std::sqrt(tone_power)}, {13, std::sqrt(tone_power)}}});
    for (const auto &entry : tones.amplitudes) {
        near(other.amplitudes.at(entry.first), entry.second);
    }
    const NonlinearTransmissionProvider &provider = amplifier;
    const auto both = MatchedPolynomialAmplifier::from_intercepts("IP2/IP3", 20., 20., 10.);
    const auto mixed =
        both.transmit({1e6, {{10, std::sqrt(tone_power)}, {13, std::sqrt(tone_power)}}});
    near(std::norm(mixed.amplitudes.at(23)) / (100. * tone_power * tone_power / .1), 1.);
    near(mixed.amplitudes.at(3), mixed.amplitudes.at(23));
    near(mixed.amplitudes.at(7), tones.amplitudes.at(7));
    const auto single = both.transmit({1e6, {{10, std::sqrt(tone_power)}}});
    near(std::norm(single.amplitudes.at(20)) / (100. * tone_power * tone_power / (.1 * 4.)), 1.);
    const auto alternate_intercepts =
        MatchedPolynomialAmplifier::from_intercepts("75", 20., 20., 10., 75.);
    const auto other_mixed = alternate_intercepts.transmit(
        {1e6, {{10, std::sqrt(tone_power)}, {13, std::sqrt(tone_power)}}});
    for (const auto &entry : mixed.amplitudes) {
        near(entry.second, other_mixed.amplitudes.at(entry.first));
    }
    rejects<std::invalid_argument>([] {
        MatchedPolynomialAmplifier::from_intercepts(
            "bad", 20., std::numeric_limits<double>::quiet_NaN(), 10.);
    });
    rejects<std::overflow_error>([] {
        MatchedPolynomialAmplifier::from_intercepts("bad", 20., -1e308, 10.);
    });
    require(!provider.transmit({1e6, {{10, .001}}}).amplitudes.empty(), "nonlinear provider");
    rejects<std::invalid_argument>([] {
        MatchedPolynomialAmplifier::from_iip3("bad", 20., 10., 0.);
    });
    rejects<std::invalid_argument>([&] {
        amplifier.transmit({1., {{0, Complex{0., 1.}}}});
    });
    rejects<std::out_of_range>([&] {
        amplifier.port(2);
    });
}
