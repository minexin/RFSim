#include "rfmodel/single_tone_amplifier.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const SoftInputLimiter limiter(.0004, .0016);
    near(limiter.limit({}), {});
    near(limiter.limit({0., .02}), {0., .02});
    const auto phase = std::polar(1., .7);
    near(limiter.limit(1e150 * phase), .04 * phase);
    const double step = 1e-8;
    require(std::abs((std::abs(limiter.limit(.02 + step)) - .02) / step - 1.) < 1e-8,
            "limiter slope must join linear branch continuously");
    for (double cap : {.0004, -1., std::numeric_limits<double>::infinity()}) {
        rejects<std::invalid_argument>([&] {
            SoftInputLimiter invalid(.0004, cap);
        });
    }
    rejects<std::invalid_argument>([&] {
        limiter.limit(1e308);
    });
    const SingleToneLimitedAmplifier model(20., 20., 23., 20., 10.);
    const SaturatingFundamentalCompression fundamental(20., 20., 23.);
    const auto pure = MatchedPolynomialAmplifier::from_intercepts("pure", 20., 20., 10.);
    const PowerWaveSpectrum input{1e6, {{10, .001 * phase}}};
    const auto low = model.transmit(input);
    const auto polynomial = pure.transmit(input);
    require(low.amplitudes.size() == 3 && low.amplitudes.count(0) == 0, "DC must be blocked");
    near(low.amplitudes.at(20), polynomial.amplitudes.at(20));
    near(low.amplitudes.at(30), polynomial.amplitudes.at(30));
    near(low.amplitudes.at(10), fundamental.transmit_fundamental(.001 * phase));
    const auto high = model.transmit({1e6, {{10, std::sqrt(.001) * phase}}});
    const auto high_pure = pure.transmit({1e6, {{10, std::sqrt(.001) * phase}}});
    require(std::norm(high.amplitudes.at(20)) < std::norm(high_pure.amplitudes.at(20)),
            "harmonic drive must limit above the knee");
    const SingleToneLimitedAmplifier alternate(20., 20., 23., 20., 10., 75.);
    const auto other = alternate.transmit(input);
    for (const auto &entry : low.amplitudes) {
        near(other.amplitudes.at(entry.first), entry.second);
    }
    require(model.transmit({1e6, {{0, 0.}, {10, 0.}}}).amplitudes.empty(),
            "zero input must be empty");
    rejects<std::invalid_argument>([&] {
        model.transmit({1e6, {{0, .1}}});
    });
    rejects<std::invalid_argument>([&] {
        model.transmit({1e6, {{10, .1}, {11, .2}}});
    });
    rejects<std::overflow_error>([&] {
        model.transmit({1., {{std::numeric_limits<int>::max(), .001}}});
    });
    rejects<std::invalid_argument>([] {
        SingleToneLimitedAmplifier invalid(20., 20., 19., 20., 10.);
    });
}
