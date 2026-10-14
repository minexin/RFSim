#include "rfmodel/conversion/frequency_conversion.hpp"
#include "rfmodel/core/power_wave_reference.hpp"
#include "rfmodel/models/frequency_dependent_cable.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <iostream>

using namespace rfmodel;

namespace {
using Wide = std::complex<long double>;
using Factor = std::vector<Complex>;

Wide wide(Complex x) {
    return {x.real(), x.imag()};
}

SMatrix zero(std::size_t n) {
    return {n, std::vector<Complex>(n * n)};
}

// Each column multiplies an independent, real, unit-variance random variable.
// This source construction and its oracle never call a production noise transform.
ConversionNoise statistics(const std::vector<Factor> &columns) {
    const auto n = columns.front().size();
    ConversionNoise result{zero(n), zero(n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            Wide c{}, p{};
            for (const auto &column : columns) {
                c += wide(column[i]) * std::conj(wide(column[j]));
                p += wide(column[i]) * wide(column[j]);
            }
            result.covariance(i, j) = {double(c.real()), double(c.imag())};
            result.complementary(i, j) = {double(p.real()), double(p.imag())};
        }
    }
    return result;
}

void compare(const SMatrix &actual, const SMatrix &expected, double tolerance = 2e-9) {
    double scale = 0.;
    for (auto x : expected.values) {
        scale = std::max(scale, std::abs(x));
    }
    require(actual.ports == expected.ports, "noise integration dimensions");
    for (std::size_t i = 0; i < actual.values.size(); ++i) {
        require(std::isfinite(actual.values[i].real()) && std::isfinite(actual.values[i].imag()),
                "noise integration finite result");
        require(std::abs(actual.values[i] - expected.values[i]) <=
                    tolerance * std::max(scale, 1e-300),
                "noise integration factor oracle mismatch");
    }
}

// Solve physical V/I equations directly at a_new=0, hence V=-Znew*I.
// b_old=S*a_old+c, and b_new=-sqrt(Re Znew)*I. The two-by-two
// determinant solve is independent of RFModel coordinate/linear-solve helpers.
Factor physical_factor(const SMatrix &s,
                       const Factor &c,
                       const std::array<Complex, 2> &old_refs,
                       const std::array<Complex, 2> &new_refs) {
    std::array<Wide, 4> m{};
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            m[2 * i + j] = -wide(s(i, j)) * (wide(old_refs[j]) - wide(new_refs[j])) /
                           (2.L * std::sqrt((long double)old_refs[j].real()));
            if (i == j) {
                m[2 * i + j] += (-wide(new_refs[i]) - std::conj(wide(old_refs[i]))) /
                                (2.L * std::sqrt((long double)old_refs[i].real()));
            }
        }
    }
    const auto determinant = m[0] * m[3] - m[1] * m[2];
    const std::array<Wide, 2> current{(wide(c[0]) * m[3] - m[1] * wide(c[1])) / determinant,
                                      (m[0] * wide(c[1]) - wide(c[0]) * m[2]) / determinant};
    Factor result(2);
    for (std::size_t i = 0; i < 2; ++i) {
        auto value = -std::sqrt((long double)new_refs[i].real()) * current[i];
        result[i] = {double(value.real()), double(value.imag())};
    }
    return result;
}

void mixed_references() {
    const SMatrix through{2, {0., 1., 1., 0.}};
    const std::array<Complex, 2> old_refs{50., 50.};
    const Factor source{{.94164149288829413, .83623143798249067},
                        {.91988246346843572, .82775335658589255}};
    const auto input = statistics({source}).covariance;
    for (const std::array<Complex, 2> refs :
         {std::array<Complex, 2>{1.3875675623858383e-5, 1.0781613647231237e-5},
          std::array<Complex, 2>{Complex{25., 18.}, Complex{110., -32.}},
          std::array<Complex, 2>{Complex{.05, .12}, Complex{.1, -.07}}}) {
        const auto expected = statistics({physical_factor(through, source, old_refs, refs)});
        const auto actual =
            renormalize_noise(through, {input}, {old_refs[0], old_refs[1]}, {refs[0], refs[1]});
        compare(actual.watts_per_hz, expected.covariance, 2e-8);
    }
}

void conversion_case(bool improper, bool dc) {
    const std::size_t n = dc ? 3 : 2;
    auto a = zero(n), b = zero(n);
    const std::size_t offset = dc ? 1 : 0;
    a(offset, offset) = 1.;
    a(offset, offset + 1) = -.99;
    a(offset + 1, offset) = 100.;
    a(offset + 1, offset + 1) = -99.99;
    if (improper) {
        b(offset, offset) = Complex{.2, -.3};
        b(offset, offset + 1) = -.99 * Complex{.2, -.3};
    }
    if (dc) {
        // A+B maps a complex carrier pair to a real DC beat fluctuation.
        a(0, 1) = b(0, 1) = .5;
        a(0, 2) = b(0, 2) = -.495;
        a(1, 0) = .2;
        b(1, 0) = .1;
    }
    Factor first(n), second(n);
    first[offset] = first[offset + 1] = 1.;
    second[offset] = second[offset + 1] = Complex{0., improper ? .3 : 1.};
    if (dc) {
        first[0] = .04;
    }
    std::vector<Factor> columns{first, second}, outputs;
    for (const auto &column : columns) {
        Factor out(n);
        for (std::size_t i = 0; i < n; ++i) {
            Wide value{};
            for (std::size_t j = 0; j < n; ++j) {
                value +=
                    wide(a(i, j)) * wide(column[j]) + wide(b(i, j)) * std::conj(wide(column[j]));
            }
            out[i] = {double(value.real()), double(value.imag())};
        }
        outputs.push_back(out);
    }
    std::vector<ConversionChannel> channels;
    for (std::size_t i = 0; i < n; ++i) {
        channels.push_back({i, int(dc ? i : i + 1)});
    }
    const FrequencyConversionModel model(1., channels, a, b);
    const auto result =
        model.analyze(Factor(n), Factor(n), statistics(columns), model.zero_noise(), {}, true);
    const auto expected = statistics(outputs);
    compare(result.outgoing_noise.covariance, expected.covariance);
    compare(result.outgoing_noise.complementary, expected.complementary);
    compare(result.incident_noise.covariance, statistics(columns).covariance);
    if (dc) {
        require(result.outgoing_noise.covariance(0, 0).imag() == 0., "real DC variance");
        require(result.outgoing_noise.complementary(0, 0) == result.outgoing_noise.covariance(0, 0),
                "DC C equals P");
    }
}

void cable_numerical_regression() {
    // Numerical regression under the existing cable approximation, not a physical
    // DC or vendor-equivalence claim. Its tiny dissipative mode is already subject
    // to the documented input PSD projection; test transfer of that accepted mode.
    FrequencyDependentCableParameters p;
    p.length_m = .27;
    p.characteristic_ohms = .2;
    p.sqrt_loss_db_per_m = 4e-8;
    const auto s = frequency_dependent_cable_s_parameters(2., p);
    const auto noise = passive_thermal_noise(s, 290.);
    const auto &c = noise.watts_per_hz;
    const double root = std::sqrt(c(0, 0).real());
    require(root > 0., "cable accepted noise mode nonzero");
    const Factor factor{root, c(1, 0) / root};
    const std::array<Complex, 2> old_refs{50., 50.}, refs{.0001, .05};
    const auto expected = statistics({physical_factor(s, factor, old_refs, refs)});
    const auto actual = renormalize_noise(s, noise, {50., 50.}, {.0001, .05});
    compare(actual.watts_per_hz, expected.covariance, 2e-7);
}
} // namespace

int main() {
    mixed_references();
    conversion_case(false, false);
    conversion_case(true, false);
    conversion_case(true, true);
    cable_numerical_regression();
    std::cout << "Noise factor integration oracles passed\n";
}
