#include "rfmodel/core/network_parameters.hpp"
#include "../support/test_support.hpp"
#include <iostream>
#include <limits>

using namespace rfmodel;
using Vector = std::vector<Complex>;

int main() {
    try {
        // The leading zero forces a row swap; complex coefficients exercise both quadratures.
        const SMatrix base{3, {0., Complex{2., 1.}, 1., 3., -1., Complex{0., 2.}, 1., 1., 4.}};
        const Vector expected{{.3, -.2}, {-1., .4}, {.7, .1}};
        for (double scale : {1e-150, 1., 1e150}) {
            auto a = base;
            for (auto &entry : a.values) {
                entry *= scale;
            }
            Vector rhs(3);
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) {
                    rhs[i] += a(i, j) * expected[j];
                }
            }
            const auto actual = parameter_detail::solve(a, rhs);
            SMatrix matrix_rhs{3, Vector(9)};
            for (std::size_t i = 0; i < 3; ++i) {
                matrix_rhs(i, 0) = rhs[i];
            }
            const auto full = parameter_detail::solve(a, matrix_rhs);
            for (std::size_t i = 0; i < 3; ++i) {
                near(actual[i], full(i, 0));
            }
            for (std::size_t i = 0; i < 3; ++i) {
                near(actual[i], expected[i]);
            }
            const auto zero = parameter_detail::solve(a, Vector(3));
            for (auto entry : zero) {
                near(entry, 0.);
            }
        }
        // A small system is valid without an absolute floor, but rejected with one.
        near(parameter_detail::solve(SMatrix{1, {1e-100}}, Vector{2e-100})[0], 2.);
        rejects<std::domain_error>([] {
            parameter_detail::solve(SMatrix{1, {1e-100}}, Vector{2e-100}, 1.);
        });
        for (const Vector &rhs : {Vector{1., 2.}, Vector(2)}) {
            rejects<std::domain_error>([&] {
                parameter_detail::solve(SMatrix{2, {1., 1., 1., 1.}}, rhs);
            });
        }
        rejects<std::overflow_error>([] {
            parameter_detail::solve(SMatrix{1, {1e-200}}, Vector{1e200});
        });
        rejects<std::invalid_argument>([] {
            parameter_detail::solve(SMatrix{2, {1.}}, Vector{1., 2.});
        });
        rejects<std::invalid_argument>([] {
            parameter_detail::solve(SMatrix{1, {1.}}, Vector{});
        });
        for (double invalid : {-1.,
                               std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
            rejects<std::invalid_argument>([&] {
                parameter_detail::solve(SMatrix{1, {1.}}, Vector{1.}, invalid);
            });
        }
        for (double invalid :
             {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            rejects<std::invalid_argument>([&] {
                parameter_detail::solve(SMatrix{1, {invalid}}, Vector{1.});
            });
            rejects<std::invalid_argument>([&] {
                parameter_detail::solve(SMatrix{1, {1.}}, Vector{invalid});
            });
        }
        // The vector primitive reports numeric range loss for callers that can retry
        // with a different operation order; it must not silently erase tiny terms.
        for (const SMatrix &a :
             {SMatrix{2, {1., 0., 1e-200, 1e-12}}, SMatrix{2, {1e-12, 1e-200, 0., 1.}}}) {
            const Vector rhs = a(0, 0) == Complex{1.} ? Vector{1e-130, 0.} : Vector{0., 1e-130};
            rejects<std::overflow_error>([&] {
                parameter_detail::solve(a, rhs);
            });
        }
        rejects<std::overflow_error>([] {
            parameter_detail::solve(SMatrix{1, {1e200}}, Vector{1e-130});
        });
        // Structural zeros in a pivoted triangular solve must not cause false residual failures.
        for (std::size_t n : {4, 7, 12}) {
            SMatrix a{n, Vector(n * n)};
            Vector rhs(n), expected_zero(n);
            rhs[0] = 1.;
            for (std::size_t i = 0; i < n; ++i) {
                a(i, i) = 1.;
                for (std::size_t j = 0; j < i; ++j) {
                    a(i, j) = (static_cast<int>((i + 3) * (j + 5) % 19) - 9) * .37;
                }
                expected_zero[i] = rhs[i];
                for (std::size_t j = 0; j < i; ++j) {
                    expected_zero[i] -= a(i, j) * expected_zero[j];
                }
            }
            const auto actual = parameter_detail::solve(a, rhs);
            for (std::size_t i = 0; i < n; ++i) {
                require(std::abs(actual[i] - expected_zero[i]) <
                            2e-12 * std::max(1., std::abs(expected_zero[i])),
                        "single RHS triangular residual regression");
            }
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
