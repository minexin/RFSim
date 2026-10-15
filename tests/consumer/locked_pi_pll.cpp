#include <rfmodel/noise/locked_pi_pll.hpp>
#include <stdexcept>
bool locked_pi_pll_probe();

int main() {
    const auto result = rfmodel::analyze_locked_pi_pll(
        {1., 1., 1., 2., 1.}, 1. / (2. * std::acos(-1.)), {1e-12, 0., 0., 0., 0.});
    const auto noise =
        rfmodel::locked_pi_pll_sidebands({{0, 9}, {0, 10}, {0, 11}},
                                         1,
                                         1.,
                                         result.offset_hz,
                                         {{1, result.offset_hz, result.noise.total_rad2_per_hz}});
    if (!locked_pi_pll_probe() ||
        std::abs(result.transfer.reference - rfmodel::Complex{1., -.5}) > 1e-12 ||
        std::abs(noise.covariance(0, 0).real() / 6.25e-13 - 1.) > 1e-12) {
        throw std::runtime_error("installed locked PI PLL contract mismatch");
    }
}
