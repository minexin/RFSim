#include "rfmodel/noise/noise_matrix.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <cstring>
#include <iostream>

namespace {
using namespace rfmodel;

void relative(Complex actual, Complex expected, double tolerance = 3e-14) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()), "nonfinite block result");
    if (!(expected == Complex{} ? actual == expected
                                : std::abs(actual / expected - 1.) < tolerance)) {
        std::cerr << "actual=" << actual << " expected=" << expected
                  << " relative=" << actual / expected - 1. << "\n";
    }
    require(expected == Complex{} ? actual == expected
                                  : std::abs(actual / expected - 1.) < tolerance,
            "independent block relative mismatch");
}

noise_detail::PsdFactor
preserves_legacy(const SMatrix &c, double floor = 0., bool unchanged = false) {
    const auto old = noise_detail::legacy_psd_factor(c, floor);
    const auto next = noise_detail::psd_factor(c, floor);
    require(std::memcmp(&old.scale, &next.scale, sizeof(double)) == 0, "old scale changed");
    require(next.rank >= old.rank && next.rank <= c.ports, "independent rank budget");
    require(!unchanged || old.rank == next.rank, "fallback changed rank");
    for (std::size_t i = 0; i < c.ports; ++i) {
        for (std::size_t k = 0; k < (unchanged ? c.ports : old.rank); ++k) {
            require(std::memcmp(&old.values(i, k), &next.values(i, k), sizeof(Complex)) == 0,
                    "old factor coefficient changed");
        }
    }
    return next;
}

SMatrix permuted(const SMatrix &c, const std::array<std::size_t, 3> &p) {
    SMatrix result{3, std::vector<Complex>(9)};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            result(i, j) = c(p[i], p[j]);
        }
    }
    return result;
}

void independent_and_correlated() {
    const SMatrix c{3, {1., 0., 0., 0., 1e-16, Complex{0., -5e-17}, 0., Complex{0., 5e-17}, 1e-16}};
    std::array<std::size_t, 3> p{0, 1, 2};
    do {
        const auto input = permuted(c, p);
        require(preserves_legacy(input).rank == 3, "correlated local rank not recovered");
        const auto reconstructed = noise_detail::psd_matrix(input);
        SMatrix h{3, std::vector<Complex>(9)};
        for (std::size_t i = 0; i < 3; ++i) {
            h(i, i) = p[i] == 0 ? 1. : 1e8;
        }
        const auto output = propagate_noise(h, {input}).watts_per_hz;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                relative(reconstructed(i, j), input(i, j));
                relative(output(i, j), h(i, i) * input(i, j) * h(j, j));
            }
        }
    } while (std::next_permutation(p.begin(), p.end()));

    // A recovered connected block still uses the old local cutoff. Its partially
    // retained weak mode is not recursively rescued by the wrapper.
    const SMatrix mixed_local{3, {1., 0., 0., 0., 1e-16, 5e-25, 0., 5e-25, 1e-32}};
    require(preserves_legacy(mixed_local).rank == 2, "local cutoff was relaxed");
    relative(noise_detail::psd_matrix(mixed_local)(2, 2), 2.5e-33);

    // Many independent singletons use every available column, including zero rows.
    constexpr std::size_t n = 64;
    SMatrix many{n, std::vector<Complex>(n * n)};
    many(17, 17) = 1.;
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 17 && i % 7 != 0) {
            many(i, i) = 1e-16;
        }
    }
    const auto factor = preserves_legacy(many);
    require(factor.rank == 54, "many-component rank budget");
    const auto reconstructed = noise_detail::psd_matrix(many);
    for (std::size_t i = 0; i < n; ++i) {
        relative(reconstructed(i, i), many(i, i));
    }
}

void admission_and_fallback() {
    // Global admission still rejects before any component recovery can run.
    for (const auto &bad : std::vector<SMatrix>{{2, {1., 0., 0., -1e-4}},
                                                {2, {1., 1e-4, 0., 1e-16}},
                                                {2, {1., 2., 2., 1.}},
                                                {2, {1., 0., 0.}},
                                                {0, {}},
                                                {1025, {}},
                                                {1, {std::numeric_limits<double>::quiet_NaN()}},
                                                {1, {std::numeric_limits<double>::infinity()}}}) {
        rejects<std::invalid_argument>([&] {
            noise_detail::legacy_psd_factor(bad);
        });
        rejects<std::invalid_argument>([&] {
            noise_detail::psd_factor(bad);
        });
    }
    const double maximum = std::numeric_limits<double>::max();
    const SMatrix overflow{1, {Complex{maximum, maximum}}};
    rejects<std::overflow_error>([&] {
        noise_detail::legacy_psd_factor(overflow);
    });
    rejects<std::overflow_error>([&] {
        noise_detail::psd_factor(overflow);
    });

    // A globally hidden local negative, indefinite or non-Hermitian block is
    // accepted exactly as before, but its failed local attempt adds nothing.
    for (const auto &c :
         std::vector<SMatrix>{{2, {1., 0., 0., -1e-16}},
                              {3, {1., 0., 0., 0., 1e-16, 2e-16, 0., 2e-16, 1e-16}},
                              {3, {1., 0., 0., 0., 1e-16, 5e-17, 0., 0., 1e-16}},
                              {2, {1., 0., 0., Complex{1e-16, 1e-16}}},
                              // Partially retained original components must remain untouched.
                              {2, {1., 5e-9, 5e-9, 1e-16}},
                              {2, {1., 2e-8, 2e-8, 1e-16}},
                              {2, {1., 1e-8, 1e-8, 0.}},
                              {2, {1., 1. + 1e-15, 1. + 1e-15, 1.}},
                              {2, {1., 1. - 1e-15, 1. - 1e-15, 1.}},
                              // Either direction of an exact nonzero input edge joins the graph.
                              {2, {1., 1e-16, 0., 1e-16}},
                              {2, {1., 0., 1e-16, 1e-16}},
                              {2, {1e-16, 1e-16, 0., 1.}},
                              {2, {1e-16, 0., 1e-16, 1.}}}) {
        preserves_legacy(c, 0., true);
    }
    const SMatrix mixed_fallback{
        4, {1., 0., 0., 0., 0., 1e-16, 2e-16, 0., 0., 2e-16, 1e-16, 0., 0., 0., 0., 1e-16}};
    require(preserves_legacy(mixed_fallback).rank == 2, "local failure blocked later recovery");
    const auto projected = noise_detail::psd_matrix(mixed_fallback);
    relative(projected(1, 1), 0.);
    relative(projected(2, 2), 0.);
    relative(projected(3, 3), 1e-16);
    for (double floor : {1., 2., -1.}) {
        preserves_legacy(SMatrix{2, {1., 0., 0., 1e-16}}, floor, true);
        if (floor > 0.) {
            preserves_legacy(SMatrix{2, {1e-16, 0., 0., -1e-16}}, floor, true);
        }
    }
    // Passivity's floor and both signs of its tiny residual remain unchanged.
    for (double s : {1., 1. - 1e-15, 1. + 1e-15}) {
        relative(passive_thermal_noise(SMatrix{1, {s}}, 300.).watts_per_hz(0, 0), 0.);
    }
    rejects<std::invalid_argument>([] {
        passive_thermal_noise(SMatrix{1, {2.}}, 0.);
    });
}

void cancellation() {
    // This particular Schur-roundoff trap produces ~1.11e-8 if a near-zero
    // residual of the retained strong component is appended as a physical mode.
    const SMatrix trap{3, {4761., 6900., 0., 6900., 10000., 0., 0., 0., 1e-100}};
    const SMatrix h{3, {100., -69., 1., 0., 0., 0., 0., 0., 1.}};
    auto strong_only = trap;
    strong_only(2, 2) = 0.;
    const auto old_output = propagate_noise(h, {strong_only}).watts_per_hz(0, 0);
    const auto output = propagate_noise(h, {trap}).watts_per_hz(0, 0);
    // Old factor rounding already leaves ~2.84e-25, far above the true 1e-100
    // independent budget. Preserve that boundary, never add a false ~1.11e-8.
    relative(output, old_output);
    require(std::abs(output) < 1e-24, "false strong-component Schur mode appended");
    relative(propagate_noise(SMatrix{3, {0., 0., 1., 0., 0., 0., 0., 0., 0.}}, {trap})
                 .watts_per_hz(0, 0),
             1e-100);
    preserves_legacy(trap);
    for (int a = 1; a <= 100; ++a) {
        for (int b = 1; b <= 100; ++b) {
            const SMatrix c{3,
                            {double(a * a),
                             double(a * b),
                             0.,
                             double(a * b),
                             double(b * b),
                             0.,
                             0.,
                             0.,
                             1e-100}};
            preserves_legacy(c);
            const SMatrix transfer{3, {double(b), double(-a), 1e50, 0., 0., 0., 0., 0., 1e50}};
            const auto out = propagate_noise(transfer, {c}).watts_per_hz;
            relative(out(0, 0), 1.);
            relative(out(2, 2), 1.);
        }
    }
    const SMatrix c{3, {1., Complex{0., -1.}, 0., Complex{0., 1.}, 1., 0., 0., 0., 1e-100}};
    const SMatrix transfer{3, {1e150, Complex{0., 1e150}, 1e50, 0., 0., 0., 0., 0., 1e50}};
    relative(propagate_noise(transfer, {c}).watts_per_hz(0, 0), 1.);
    preserves_legacy(c);
}

void range_pair(int a, int b) {
    const SMatrix c{2, {std::scalbn(1., a), 0., 0., std::scalbn(1., b)}};
    const SMatrix h{2, {std::scalbn(1., -a / 2), 0., 0., std::scalbn(1., -b / 2)}};
    const bool supported = std::abs(a - b) <= 1022;
    preserves_legacy(c, 0., !supported);
    const auto out = propagate_noise(h, {c}).watts_per_hz;
    relative(out(0, 0), supported || a > b ? std::scalbn(1., a - 2 * (a / 2)) : 0.);
    relative(out(1, 1), supported || b > a ? std::scalbn(1., b - 2 * (b / 2)) : 0.);
}

void range_boundaries() {
    for (int a = -1074; a <= 1023; a += 17) {
        for (int b = -1074; b <= 1023; b += 19) {
            range_pair(a, b);
        }
    }
    for (int b = -1074; b <= 0; ++b) {
        range_pair(0, b);
    }
    // The block maximum alone is insufficient: every nonzero real AND imaginary
    // input part must stay normal under the original global scale.
    for (const auto &c : std::vector<SMatrix>{
             {3, {1e300, 0., 0., 0., 1e-7, 1e-14, 0., 1e-14, 1e-17}},
             {3, {1e300, 0., 0., 0., 1e-7, Complex{1e-8, 1e-14}, 0., Complex{1e-8, -1e-14}, 1e-7}},
             {2, {1., 0., 0., std::numeric_limits<double>::denorm_min()}}}) {
        preserves_legacy(c, 0., true);
    }
    // Local Cholesky fill-in can leave the normal range even though every
    // original nonzero part/global scale is normal. Reject the entire append.
    for (double edge_ratio : {1e-100, 1e-74}) {
        SMatrix c{5, std::vector<Complex>(25)};
        c(0, 0) = 1.;
        for (std::size_t i = 1; i < 5; ++i) {
            c(i, i) = 1e-200;
        }
        for (auto edge : {std::pair<std::size_t, std::size_t>{1, 2}, {1, 3}, {2, 4}}) {
            c(edge.first, edge.second) = c(edge.second, edge.first) = 1e-200 * edge_ratio;
        }
        for (auto value : c.values) {
            require(value == Complex{} || std::isnormal(value.real()), "input must be in range");
        }
        preserves_legacy(c, 0., true);
        SMatrix separate{6, std::vector<Complex>(36)};
        for (std::size_t i = 0; i < 5; ++i) {
            for (std::size_t j = 0; j < 5; ++j) {
                separate(i, j) = c(i, j);
            }
        }
        separate(5, 5) = 1e-16;
        const auto factor = preserves_legacy(separate);
        require(factor.rank == 2, "one component fallback blocked another recovery");
        const auto reconstructed = noise_detail::psd_matrix(separate);
        for (std::size_t i = 1; i < 5; ++i) {
            relative(reconstructed(i, i), 0.);
        }
        relative(reconstructed(5, 5), 1e-16);
    }
    // Real and imaginary fill-in exactly at min-normal is eligible; one exponent
    // step below is not. A later independent block proves atomic local fallback.
    for (bool imaginary : {false, true}) {
        for (int exponent : {-240, -241, -260}) {
            const double scale = std::scalbn(1., -604);
            const double edge = scale * std::scalbn(1., exponent);
            SMatrix c{6, std::vector<Complex>(36)};
            c(0, 0) = 1.;
            c(5, 5) = 1e-100;
            for (std::size_t i = 1; i < 5; ++i) {
                c(i, i) = scale;
            }
            for (auto endpoints : {std::pair<std::size_t, std::size_t>{1, 2}, {1, 3}, {2, 4}}) {
                const Complex value = imaginary ? Complex{0., edge} : Complex{edge, 0.};
                c(endpoints.first, endpoints.second) = value;
                c(endpoints.second, endpoints.first) = std::conj(value);
            }
            const auto factor = preserves_legacy(c);
            require(factor.rank == (exponent == -240 ? 6u : 2u), "fill min-normal boundary");
            if (exponent != -240) {
                for (std::size_t i = 1; i < 5; ++i) {
                    for (std::size_t k = 0; k < factor.rank; ++k) {
                        require(factor.values(i, k) == Complex{}, "partial component appended");
                    }
                }
            }
        }
    }
}
} // namespace

int main() {
    independent_and_correlated();
    admission_and_fallback();
    cancellation();
    range_boundaries();
    std::cout
        << "Independent weak-component contract, 10000 cancellation cases and range sweep passed\n";
}
