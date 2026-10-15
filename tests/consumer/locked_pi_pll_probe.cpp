#include <rfmodel/locked_pi_pll.hpp>

bool locked_pi_pll_probe() {
    const auto result = rfmodel::analyze_locked_pi_pll({}, 1.);
    return result.noise.total_rad2_per_hz == 0. &&
           std::abs(result.transfer.reference + result.transfer.vco - 1.) < 1e-12;
}
