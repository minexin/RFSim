#include "rfmodel/conversion_network.hpp"
#include "rfmodel/mixer_linearization.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>

int main() {
    try {
        using namespace rfmodel;
        const Complex a{.3, .1}, b{.12, -.04}, gamma{.2, .1};
        const Complex source{.4, -.2}, offset{-.3, .5};
        FrequencyConversionModel model(1., {{0, 1}}, {1, {a}}, {1, {b}});
        const auto zero = model.zero_noise();
        const ConversionNoise noise{{1, {1.}}, {1, {Complex{0., .2}}}};
        const auto ordinary = model.analyze({source}, {gamma}, noise, zero, {}, true);
        const auto result = model.analyze({source}, {gamma}, noise, zero, {}, true, {offset});
        const auto u = 1. - a * gamma, q = b * std::conj(gamma);
        const auto z = a * source + b * std::conj(source) + offset;
        const auto expected = (std::conj(u) * z + q * std::conj(z)) / (std::norm(u) - std::norm(q));
        near(result.outgoing[0], expected);
        near(result.incident[0], source + gamma * expected);
        near(result.outgoing_noise.covariance(0, 0), ordinary.outgoing_noise.covariance(0, 0));
        near(result.outgoing_noise.complementary(0, 0),
             ordinary.outgoing_noise.complementary(0, 0));
        require(result.relative_residual < 1e-12, "affine residual differs");
        rejects<std::invalid_argument>([&] {
            model.analyze({source}, {gamma}, zero, zero, {}, false, {0., 0.});
        });
        rejects<std::invalid_argument>([&] {
            model.analyze({source},
                          {gamma},
                          zero,
                          zero,
                          {},
                          false,
                          {std::numeric_limits<double>::infinity()});
        });
        FrequencyConversionNetwork network(1.);
        network.add(FrequencyConversionModel(1., {{0, 1}}, {1, {0.}}, {1, {0.}}));
        network.add(FrequencyConversionModel(1., {{0, 1}}, {1, {.5}}, {1, {0.}}));
        network.connect(0, 0, 1, 0);
        const auto emitted = network.analyze(false, nullptr, {1., 0.});
        near(emitted.outgoing[0], 1.);
        near(emitted.outgoing[1], .5);
        near(emitted.incident[0], .5);
        near(emitted.incident[1], 1.);
        const std::vector<Complex> operating{1., 2., 0., 0.};
        const auto mixer =
            linearize_real_mixer(1., {{0, 12}, {1, 10}, {2, 2}, {2, 22}}, operating, 10, 0.);
        const auto silent = mixer.incremental_model.zero_noise();
        const auto nominal = mixer.incremental_model.analyze(
            operating, std::vector<Complex>(4), silent, silent, {}, false, mixer.output_offset());
        for (std::size_t i = 0; i < 4; ++i) {
            near(nominal.outgoing[i], mixer.operating_outgoing[i]);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
