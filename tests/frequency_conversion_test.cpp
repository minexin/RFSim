#include "rfmodel/frequency_conversion.hpp"
#include "rfmodel/ideal_mixer.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    const std::vector<ConversionChannel> channels{
        {0, 8}, {0, 10}, {0, 12}, {0, 0}, {1, 2}, {1, 18}, {1, 20}, {1, 22}, {1, 0}, {1, 10}};
    const auto mixer = ideal_mixer_conversion(1e8, channels, 10, -3., .4);
    auto zero = mixer.zero_noise();
    std::vector<Complex> source(channels.size()), reflection(channels.size());
    source[0] = {1., 2.};
    source[1] = {.7, -.3};
    source[2] = {-.4, .8};
    source[3] = .9;
    const auto result = mixer.analyze(source, reflection, zero, zero);
    IdealRealMixer existing("existing", 10, -3., .4);
    const auto expected = existing.transmit(
        {1e8, {{8, source[0]}, {10, source[1]}, {12, source[2]}, {0, source[3]}}});
    for (std::size_t i = 4; i < channels.size(); ++i) {
        near(result.outgoing[i], expected.amplitudes.at(channels[i].bin));
    }
    require(result.relative_residual < 1e-14, "conversion wave residual");
    // One independent real noise basis vector at a time, including improper RF
    // statistics, verifies the full complex C/P matrices without Monte Carlo.
    auto noise = zero;
    std::vector<std::vector<Complex>> bases{
        {{1., .2}, {.3, .1}, {-.5, .7}, .4, 0., 0., 0., 0., 0., 0.},
        {{.2, 1.}, {-.4, .8}, {.3, -.2}, -.7, 0., 0., 0., 0., 0., 0.}};
    auto expected_noise = zero;
    for (const auto &basis : bases) {
        for (std::size_t i = 0; i < channels.size(); ++i) {
            for (std::size_t j = 0; j < channels.size(); ++j) {
                noise.covariance(i, j) += basis[i] * std::conj(basis[j]);
                noise.complementary(i, j) += basis[i] * basis[j];
            }
        }
        const auto mapped = mixer.analyze(basis, reflection, zero, zero).outgoing;
        for (std::size_t i = 0; i < channels.size(); ++i) {
            for (std::size_t j = 0; j < channels.size(); ++j) {
                expected_noise.covariance(i, j) += mapped[i] * std::conj(mapped[j]);
                expected_noise.complementary(i, j) += mapped[i] * mapped[j];
            }
        }
    }
    const auto noisy = mixer.analyze(source, reflection, noise, zero);
    for (std::size_t i = 0; i < noise.covariance.values.size(); ++i) {
        near(noisy.outgoing_noise.covariance.values[i], expected_noise.covariance.values[i]);
        near(noisy.outgoing_noise.complementary.values[i], expected_noise.complementary.values[i]);
    }
    FrequencyConversionModel feedback(1e9, {{0, 1}}, {1, {.2}}, {1, {.1}});
    auto proper = feedback.zero_noise();
    proper.covariance(0, 0) = 2.;
    const auto closed = feedback.analyze({{2., 3.}}, {.5}, proper, feedback.zero_noise());
    near(closed.outgoing[0], Complex{.3 * 2. / .85, .1 * 3. / .95});
    near(closed.outgoing_noise.covariance(0, 0), std::pow(.3 / .85, 2) + std::pow(.1 / .95, 2));
    near(closed.outgoing_noise.complementary(0, 0), std::pow(.3 / .85, 2) - std::pow(.1 / .95, 2));
    rejects<std::invalid_argument>([&] {
        auto bad = proper;
        bad.complementary(0, 0) = 3.;
        feedback.analyze({0.}, {0.}, bad, feedback.zero_noise());
    });
    rejects<std::domain_error>([] {
        FrequencyConversionModel singular(1., {{0, 1}}, {1, {1.}}, {1, {0.}});
        auto z = singular.zero_noise();
        singular.analyze({1.}, {1.}, z, z);
    });
    rejects<std::invalid_argument>([] {
        ideal_mixer_conversion(1., {{0, 8}, {1, 2}}, 10, 0.);
    });
    rejects<std::invalid_argument>([] {
        FrequencyConversionModel bad(1., {{0, 0}}, {1, {Complex{0., 1.}}}, {1, {0.}});
    });
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
