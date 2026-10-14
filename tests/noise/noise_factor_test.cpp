#include "rfmodel/noise/noise_matrix.hpp"
#include "../support/test_support.hpp"
#include <limits>

namespace {
using namespace rfmodel;

void relative(Complex actual, Complex expected, double tolerance = 2e-12) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()),
            "nonfinite factor result");
    if (expected == Complex{}) {
        require(actual == Complex{}, "nullspace not preserved");
    } else {
        require(std::abs(actual / expected - 1.) < tolerance, "scaled factor mismatch");
    }
}

void factor_oracle(const SMatrix &u, const SMatrix &t, double scale = 1.) {
    const auto n = u.ports;
    SMatrix c{n, std::vector<Complex>(n * n)}, q = c;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            for (std::size_t k = 0; k < n; ++k) {
                c(i, j) += u(i, k) * std::conj(u(j, k)) * scale;
                q(i, j) += t(i, k) * u(k, j);
            }
        }
    }
    const auto actual = propagate_noise(t, {c}).watts_per_hz;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            Complex expected = 0.;
            for (std::size_t k = 0; k < n; ++k) {
                expected += q(i, k) * std::conj(q(j, k)) * scale;
            }
            relative(actual(i, j), expected);
            require(actual(i, j) == std::conj(actual(j, i)), "not exactly Hermitian");
        }
    }
}
} // namespace

int main() {
    using namespace rfmodel;
    // Independent known U, never the implementation's Cholesky reconstruction.
    for (double epsilon : {.1, .01, .001, .0001, .00001}) {
        for (double scale : {1., 1e-280, 1e280}) {
            factor_oracle(SMatrix{2, {1., 0., 1., 0.}},
                          SMatrix{2, {1., -1. + epsilon, 100., -100. + epsilon}},
                          scale);
        }
    }
    // Complex rank-one, largest pivot last, a completely zero input port.
    factor_oracle(SMatrix{3, {Complex{0., 1.}, 0., 0., 0., 0., 0., 2., 0., 0.}},
                  SMatrix{3, {2., 1., Complex{0., -1.}, 1., 2., 3., 0., 1., 1.}});
    // Rank two, port permutations, nonzero nullspace.
    factor_oracle(SMatrix{3, {1., 1., 0., 0., 2., 0., 2., 0., 0.}},
                  SMatrix{3, {2., -1., -1., 1., 2., 3., Complex{0., 1.}, 0., 1.}});
    // Two admitted modes must not be reprojected relative to amplified output.
    const auto modes =
        propagate_noise(SMatrix{2, {1e100, 0., 0., 1.}}, {SMatrix{2, {1., 0., 0., 1e-10}}})
            .watts_per_hz;
    relative(modes(0, 0), 1e200);
    relative(modes(1, 1), 1e-10);
    // Input tiny-mode truncation remains exactly the documented admission rule.
    const auto discarded =
        propagate_noise(SMatrix{2, {1., 0., 0., 1e100}}, {SMatrix{2, {1., 0., 0., 1e-16}}})
            .watts_per_hz;
    relative(discarded(1, 1), 0.);
    const auto zero =
        propagate_noise(SMatrix{2, {1e308, 1e308, -1e308, 1e308}}, {SMatrix{2, {0., 0., 0., 0.}}})
            .watts_per_hz;
    for (auto x : zero.values) {
        relative(x, 0.);
    }
    // Huge intermediate covariance and underflowing intermediate covariance,
    // but finite representable final values, including subnormal input/output.
    for (auto pair : {std::pair<double, double>{1e-300, 1e300},
                      {1e300, 1e-300},
                      {std::numeric_limits<double>::denorm_min(), 1e150},
                      {1., 1e-160}}) {
        const double c = pair.first, t = pair.second;
        const auto actual = propagate_noise(SMatrix{1, {t}}, {SMatrix{1, {c}}}).watts_per_hz(0, 0);
        relative(actual, (c * t) * t, 5e-4); // final subnormal rounding
    }
    // A common row scaling would erase the only surviving term before the
    // 1e308 terms cancel. Exponent-aware products preserve it.
    const auto dynamic =
        propagate_noise(SMatrix{3, {1e308, -1e308, 1e-100, 1., -1., 0., 0., 0., 1e-100}},
                        {SMatrix{3, std::vector<Complex>(9, 1e300)}})
            .watts_per_hz;
    relative(dynamic(0, 0), 1e100);
    relative(dynamic(0, 2), 1e100);
    relative(dynamic(1, 1), 0.);
    const SMatrix identity{2, {1., 0., 0., 1.}};
    for (const auto &bad : std::vector<SMatrix>{{2, {1., 1., 0., 1.}},
                                                {2, {1., 2., 2., 1.}},
                                                {2, {1., 0., 0., -1.}},
                                                {2, {1., 0., 0.}},
                                                {0, {}},
                                                {1, {std::numeric_limits<double>::quiet_NaN()}},
                                                {1, {std::numeric_limits<double>::infinity()}}}) {
        rejects<std::invalid_argument>([&] {
            propagate_noise(identity, {bad});
        });
    }
    rejects<std::invalid_argument>([&] {
        propagate_noise(SMatrix{1, {1.}}, {identity});
    });
    rejects<std::invalid_argument>([&] {
        propagate_noise(SMatrix{2, {1., 0., 0., std::numeric_limits<double>::infinity()}},
                        {identity});
    });
    rejects<std::invalid_argument>([] {
        propagate_noise(SMatrix{1, {1e308}}, {SMatrix{1, {1e308}}});
    });
    rejects<std::invalid_argument>([] {
        passive_thermal_noise(SMatrix{1, {2.}}, 0.);
    });
}
