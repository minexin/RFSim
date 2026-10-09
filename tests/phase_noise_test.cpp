#include "rfmodel/phase_noise.hpp"
#include <iostream>

int main() {
    try {
        const std::vector<rfmodel::ConversionChannel> channels{{0, 9}, {0, 10}, {0, 11}};
        const auto noise = rfmodel::phase_noise_sidebands(channels, 1, {1., 2.}, {{1, -100.}});
        if (std::abs(noise.covariance(0, 0).real() / 5e-10 - 1.) > 1e-13 ||
            std::abs(noise.complementary(0, 2) / rfmodel::Complex{3e-10, -4e-10} - 1.) > 1e-13 ||
            noise.covariance(0, 2) != rfmodel::Complex{} ||
            noise.complementary(0, 0) != rfmodel::Complex{}) {
            throw std::runtime_error("phase noise sideband statistics differ");
        }
        auto identity = rfmodel::conversion_detail::zero(3);
        for (std::size_t i = 0; i < 3; ++i) {
            identity(i, i) = 1.;
        }
        const rfmodel::FrequencyConversionModel model(
            1., channels, identity, rfmodel::conversion_detail::zero(3));
        const auto result = model.analyze(
            std::vector<rfmodel::Complex>(3),
            std::vector<rfmodel::Complex>(3),
            noise,
            {rfmodel::conversion_detail::zero(3), rfmodel::conversion_detail::zero(3)});
        if (std::abs(result.outgoing_noise.complementary(0, 2) - noise.complementary(0, 2)) >
            1e-22) {
            throw std::runtime_error("phase noise propagation differs");
        }
        bool rejected = false;
        try {
            rfmodel::phase_noise_sidebands(channels, 1, 1., {{10, -100.}});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("DC crossing was accepted");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
