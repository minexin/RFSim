#include <rfmodel/noise/noise_matrix.hpp>
#include <cmath>
#include <stdexcept>
rfmodel::NoiseCorrelation installed_noise_factor_probe();

int main() {
    const auto result = installed_noise_factor_probe().watts_per_hz;
    const double q[] = {1. - .99, 100. - 99.99};
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            if (std::abs(result(i, j) / (q[i] * q[j]) - 1.) > 1e-12) {
                throw std::runtime_error("installed factor cancellation mismatch");
            }
        }
    }
    const auto extreme =
        rfmodel::propagate_noise(rfmodel::SMatrix{1, {1e300}}, {rfmodel::SMatrix{1, {1e-300}}});
    if (std::abs(extreme.watts_per_hz(0, 0) / 1e300 - 1.) > 1e-12) {
        throw std::runtime_error("installed factor scaling mismatch");
    }
}
