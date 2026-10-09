#include "rfmodel/network_parameters.hpp"
#include "test_support.hpp"
#include <iostream>

using namespace rfmodel;

int main() {
    try {
        // Exact triangular systems whose pivoted inverse has structural zeros.
        // A nonzero floating residue in one such entry must be judged relative
        // to its equation and RHS solution, not relative only to that residue.
        for (std::size_t n : {4, 7, 12}) {
            SMatrix a{n, std::vector<Complex>(n * n)}, rhs = a, expected = a;
            for (std::size_t i = 0; i < n; ++i) {
                a(i, i) = 1.;
                rhs(i, i) = i % 3 == 0 ? 1e-120 : (i % 3 == 1 ? 1. : 1e120);
                for (std::size_t j = 0; j < i; ++j) {
                    a(i, j) = (static_cast<int>((i + 3) * (j + 5) % 19) - 9) * .37;
                }
            }
            // Independent forward substitution, without pivoting or inversion.
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    expected(i, j) = rhs(i, j);
                    for (std::size_t k = 0; k < i; ++k) {
                        expected(i, j) -= a(i, k) * expected(k, j);
                    }
                }
            }
            const auto actual = parameter_detail::solve(a, rhs);
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    const double rhs_scale = std::abs(rhs(j, j));
                    require(std::abs((actual(i, j) - expected(i, j)) / rhs_scale) <
                                2e-12 * std::max(1., std::abs(expected(i, j) / rhs_scale)),
                            "pivoted solve differs from independent triangular solution");
                }
            }
        }
        rejects<std::domain_error>([] {
            parameter_detail::solve({2, {1., 1., 1., 1.}}, {2, {1., 0., 0., 1.}});
        });
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
