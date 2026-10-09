#include "rfmodel/mixer_linearization.hpp"
#include <iostream>

int main() {
    try {
        const auto result = rfmodel::linearize_real_mixer(
            1e6, {{0, 12}, {1, 10}, {2, 2}, {2, 22}}, {1., 2., 0., 0.}, 10, 0.);
        if (std::abs(result.operating_outgoing[2] - rfmodel::Complex{1., 0.}) > 1e-13 ||
            std::abs(result.incremental_model.direct()(2, 0) - rfmodel::Complex{1., 0.}) > 1e-13 ||
            std::abs(result.incremental_model.conjugate()(2, 1) - rfmodel::Complex{.5, 0.}) >
                1e-13 ||
            std::abs(result.incremental_model.direct()(3, 1) - rfmodel::Complex{.5, 0.}) > 1e-13) {
            throw std::runtime_error("RF/LO derivative or nominal output differs");
        }
        const auto noise = result.incremental_model.zero_noise();
        const auto delta = result.incremental_model.analyze(
            {0., .02, 0., 0.}, std::vector<rfmodel::Complex>(4), noise, noise);
        if (std::abs(delta.outgoing[2] - rfmodel::Complex{.01, 0.}) > 1e-13) {
            throw std::runtime_error("LO incremental gain differs");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
