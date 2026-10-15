#include "rfmodel/mna/linear_network.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace rfmodel::mna;
std::size_t checks = 0;

void require(bool value, const char *message) {
    ++checks;
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class F> void fails(F action, Failure expected) {
    try {
        action();
    } catch (const SolveError &error) {
        require(error.failure() == expected, "shared kernel reported unexpected failure");
        return;
    }
    throw std::runtime_error("shared kernel unexpectedly accepted mutation/range fixture");
}

void equal_diagnostics(const Diagnostics &a, const Diagnostics &b) {
    require(a.max_scaled_residual == b.max_scaled_residual &&
                a.max_kcl_residual_a == b.max_kcl_residual_a &&
                a.max_constraint_residual_v == b.max_constraint_residual_v &&
                a.min_scaled_pivot == b.min_scaled_pivot &&
                a.effective_pivot_tolerance == b.effective_pivot_tolerance &&
                a.refinement_steps == b.refinement_steps &&
                a.min_equilibration_exponent == b.min_equilibration_exponent &&
                a.max_equilibration_exponent == b.max_equilibration_exponent,
            "shared factorization and legacy single-RHS diagnostics agree exactly");
}

void sequential_row_scaling() {
    SolveOptions options;
    options.voltage_scale_v = options.current_scale_a = 1;
    const double large = std::scalbn(1., 1000), middle = std::scalbn(1., 200);
    const std::vector<Complex> matrix{middle, middle, large, large / 2};
    const std::vector<Complex> rhs{std::scalbn(1., -950), large};
    const auto factor = detail::factor_matrix(matrix, 2, 2, options);
    // The first product is outside the supported normal-or-zero domain. Summing
    // these exponents before scaling would wrongly accept a rounded x=(2,-2).
    const std::vector<int> expected{-128, 56, 128, 128};
    for (std::size_t pass = 0; pass < 4; ++pass) {
        require(std::ilogb(factor.row_scales[pass * 2]) == expected[pass],
                "fixture exercises opposing sequential row scaling exponents");
    }
    fails(
        [&] {
            (void)detail::solve_factored(factor, rhs);
        },
        Failure::range_error);
    fails(
        [&] {
            (void)detail::solve_matrix(matrix, rhs, 2, options);
        },
        Failure::range_error);
}

void refinement_permutations_and_reuse() {
    SolveOptions options;
    options.voltage_scale_v = options.current_scale_a = 1;
    options.backward_relative_tolerance = std::numeric_limits<double>::epsilon() / 4;
    // Independent exact multiplication A*(1,-2,3)=(18,-15,-16).
    const std::vector<Complex> matrix{1, -7, 1, 0, 0, -5, 6, -1, -8};
    const std::vector<Complex> rhs{18, -15, -16};
    const auto factor = detail::factor_matrix(matrix, 3, 3, options);
    const auto prepared = detail::prepare_rhs(factor, rhs);
    require(factor.pivots == std::vector<std::size_t>{2, 2, 2},
            "refinement fixture contains a later row permutation");
    const auto solved = detail::solve_factored(factor, rhs, prepared);
    require(solved.x == std::vector<Complex>{1, -2, 3}, "refinement recovers the exact answer");
    require(solved.diagnostics.refinement_steps == 1 && solved.diagnostics.max_scaled_residual == 0,
            "fixture actually exercises refinement and strict revalidation");
    const auto legacy = detail::solve_matrix(matrix, rhs, 3, options);
    require(solved.x == legacy.x, "legacy and factor replay preserve all solution bits");
    equal_diagnostics(solved.diagnostics, legacy.diagnostics);

    // Prepare the initial solution using correct chronological multipliers;
    // corrupt only the subsequent refinement permutation to prove sensitivity.
    auto wrong = factor;
    wrong.pivots[1] = 1;
    fails(
        [&] {
            (void)detail::solve_factored(wrong, rhs, prepared);
        },
        Failure::residual_failure);

    // Reuse one const factor for independent RHSs, including zero. No previous
    // column's solution, refinement counter, or residual can leak into the next.
    for (const auto &column :
         std::vector<std::vector<Complex>>{rhs, {0, 0, 0}, {-18, 15, 16}, {9, -7.5, -8}, rhs}) {
        const auto actual = detail::solve_factored(factor, column);
        const auto expected = detail::solve_matrix(matrix, column, 3, options);
        require(actual.x == expected.x, "reused factor reproduces each legacy RHS exactly");
        equal_diagnostics(actual.diagnostics, expected.diagnostics);
    }
    const auto zero = detail::solve_factored(factor, std::vector<Complex>(3));
    require(zero.diagnostics.refinement_steps == 0 && zero.diagnostics.max_scaled_residual == 0 &&
                zero.diagnostics.max_kcl_residual_a == 0 &&
                zero.diagnostics.max_constraint_residual_v == 0,
            "homogeneous RHS has its own zero diagnostics");
}
} // namespace

int main() {
    try {
        sequential_row_scaling();
        refinement_permutations_and_reuse();
        std::cout << "MNA shared factorization: " << checks << " always-active checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
