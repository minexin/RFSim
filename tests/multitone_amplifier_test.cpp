#include "rfmodel/multitone_amplifier.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const MultiToneLimitedAmplifier model(20., 20., 23., 20., 10.);
    const Complex a = std::polar(.001, .3);
    const Complex b = std::polar(.002, -.7);
    const auto low = model.evaluate({1e8, {{10, a}, {11, b}}});
    near(low.total_input_power_w, 5e-6);
    near(low.limited_input_power_w, 5e-6);
    // Independent analytic phase/amplitude laws with unequal input powers.
    const double quadratic = 10. / std::sqrt(.1);
    const double cubic = -10. / .01;
    near(low.second_order.amplitudes.at(20), quadratic / 2 * a * a);
    near(low.second_order.amplitudes.at(1), quadratic * std::conj(a) * b);
    near(low.second_order.amplitudes.at(21), quadratic * a * b);
    near(low.third_order.amplitudes.at(9), cubic * a * a * std::conj(b));
    near(low.third_order.amplitudes.at(30), cubic / 3 * a * a * a);
    near(low.third_order.amplitudes.at(10), cubic * a * (std::norm(a) + 2 * std::norm(b)));
    require(low.direct.amplitudes.count(10) && low.third_order.amplitudes.count(10),
            "overlapping direct and cubic carriers must both survive");
    require(!low.second_order.amplitudes.count(0), "generated DC must be blocked");
    near(low.direct.amplitudes.at(10) / a, low.direct.amplitudes.at(11) / b);

    const MultiToneLimitedAmplifier alternate(20., 20., 23., 20., 10., 75.);
    const auto other = alternate.evaluate({1e8, {{10, a}, {11, b}}});
    for (const auto &entry : low.second_order.amplitudes) {
        near(other.second_order.amplitudes.at(entry.first), entry.second);
    }
    for (const auto &entry : low.third_order.amplitudes) {
        near(other.third_order.amplitudes.at(entry.first), entry.second);
    }
    const auto pure = MatchedPolynomialAmplifier::from_intercepts("pure", 20., 20., 10.);
    const PowerWaveSpectrum input{1e8, {{10, a}, {11, b}}};
    auto combined = pure.homogeneous_component(1).transmit(input);
    for (std::size_t order : {0, 2, 3}) {
        for (const auto &entry : pure.homogeneous_component(order).transmit(input).amplitudes) {
            combined.amplitudes[entry.first] += entry.second;
        }
    }
    for (const auto &entry : pure.transmit(input).amplitudes) {
        near(combined.amplitudes.at(entry.first), entry.second);
    }
    require(pure.homogeneous_component(9).transmit(input).amplitudes.empty(),
            "absent polynomial orders must be zero");
    rejects<std::invalid_argument>([&] {
        pure.homogeneous_component(10);
    });

    // Reference-independent high-drive checks: common scaling, saturation and
    // continued source phase preservation, including multiple carrier bins.
    const auto high = model.evaluate({1e8, {{10, 1e100 * a}, {11, 1e100 * b}}});
    near(high.limited_input_power_w, std::pow(10., -2.8));
    const double direct_power =
        std::norm(high.direct.amplitudes.at(10)) + std::norm(high.direct.amplitudes.at(11));
    near(direct_power, std::pow(10., -.7));
    near(high.direct.amplitudes.at(10) / a, high.direct.amplitudes.at(11) / b);
    const SingleToneLimitedAmplifier single(20., 20., 23., 20., 10.);
    const PowerWaveSpectrum one_tone{1e8, {{10, .03 * std::polar(1., .4)}}};
    const auto original = single.transmit(one_tone);
    const auto separated = model.evaluate(one_tone);
    near(separated.direct.amplitudes.at(10), original.amplitudes.at(10));
    near(separated.second_order.amplitudes.at(20), original.amplitudes.at(20));
    near(separated.third_order.amplitudes.at(30), original.amplitudes.at(30));
    require(separated.third_order.amplitudes.count(10) == 1,
            "self cubic product must be explicit, not silently discarded");
    const auto empty = model.evaluate({1e8, {{0, 0.}, {10, 0.}}});
    require(empty.direct.amplitudes.empty() && empty.second_order.amplitudes.empty() &&
                empty.third_order.amplitudes.empty() && empty.total_input_power_w == 0.,
            "zero RF input must produce empty families");
    for (const PowerWaveSpectrum invalid : {PowerWaveSpectrum{1e8, {{0, .1}}},
                                            PowerWaveSpectrum{0., {}},
                                            PowerWaveSpectrum{1e8, {{-1, .1}}}}) {
        rejects<std::invalid_argument>([&] {
            model.evaluate(invalid);
        });
    }
    rejects<std::overflow_error>([&] {
        model.evaluate({1e8, {{10, 1e308}}});
    });
    rejects<std::overflow_error>([&] {
        model.evaluate({1., {{std::numeric_limits<int>::max(), .001}}});
    });
    rejects<std::invalid_argument>([] {
        MultiToneLimitedAmplifier invalid(20., 20., 19., 20., 10.);
    });
}
