#include "rfmodel/conversion_network.hpp"
#include "rfmodel/phase_noise.hpp"
#include <iostream>

int main() {
    try {
        using namespace rfmodel;
        const std::vector<ConversionChannel> channels{
            {0, 9}, {0, 10}, {0, 11}, {1, 9}, {1, 10}, {1, 11}};
        const auto noise = phase_noise_group(channels, {{1, 1., 1.}, {4, 1., -2.}}, {{1, -100.}});
        if (std::abs(noise.covariance(2, 5).real() / -2e-10 - 1.) > 1e-12 ||
            std::abs(noise.complementary(2, 3).real() / 2e-10 - 1.) > 1e-12) {
            throw std::runtime_error("shared clock cross statistics differ");
        }
        FrequencyConversionNetwork network(1.);
        for (int i = 0; i < 2; ++i) {
            network.add(FrequencyConversionModel(1., {{0, 1}}, {1, {.5}}, {1, {0.}}));
        }
        const ConversionNoise extra{{2, {1., .5, .5, 1.}}, {2, {0., 0., 0., 0.}}};
        const auto result = network.analyze(true, &extra);
        if (std::abs(result.outgoing_noise.covariance(0, 1).real() - .125) > 1e-13 ||
            std::abs(result.incident_outgoing_noise.covariance(0, 1).real() - .25) > 1e-13) {
            throw std::runtime_error("cross-device noise propagation differs");
        }
        auto invalid = extra;
        invalid.covariance(0, 0) = -1.;
        bool rejected = false;
        try {
            network.analyze(false, &invalid);
        } catch (const std::exception &) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("non-PSD independent contribution accepted");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
