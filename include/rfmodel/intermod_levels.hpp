#pragma once
#include "polynomial_intercepts.hpp"

namespace rfmodel {
// Output-referred, equal-tone homogeneous IM1..IM11 powers in dBm.
// The reference products follow SystemVue 2023 RFAMP_HO's documented table.
// Power does not determine phase: a2..an signs are always caller supplied.
inline std::vector<double>
polynomial_coefficients_from_intermod_levels(double power_gain_db,
                                             const std::vector<double> &output_levels_dbm,
                                             const std::vector<int> &coefficient_signs,
                                             double reference_ohms = 50.) {
    if (output_levels_dbm.empty() || output_levels_dbm.size() > maximum_polynomial_order ||
        coefficient_signs.size() != output_levels_dbm.size() - 1) {
        throw std::invalid_argument("expected IM1..IMn and one sign for each nonlinear order");
    }
    for (double level : output_levels_dbm) {
        if (!std::isfinite(level)) {
            throw std::invalid_argument("intermod output levels must be finite dBm values");
        }
    }
    constexpr std::array<std::array<int, 2>, 10> products{
        {{1, -1}, {2, -1}, {3, -1}, {3, -2}, {4, -2}, {4, -3}, {5, -3}, {5, -4}, {6, -4}, {6, -5}}};
    std::vector<TwoToneIntercept> intercepts;
    const double fundamental = output_levels_dbm.front();
    for (std::size_t index = 1; index < output_levels_dbm.size(); ++index) {
        const double output_ip =
            fundamental + (fundamental - output_levels_dbm[index]) / static_cast<double>(index);
        if (!std::isfinite(output_ip)) {
            throw std::overflow_error("intermod output intercept is not representable");
        }
        const auto &product = products[index - 1];
        intercepts.push_back({product[0],
                              product[1],
                              output_ip,
                              coefficient_signs[index - 1],
                              InterceptReference::output});
    }
    return polynomial_coefficients_from_intercepts(power_gain_db, intercepts, reference_ohms);
}
} // namespace rfmodel
