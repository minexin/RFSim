#include "rfmodel/adapters/model_adapter.hpp"
#include "rfmodel/io/tabulated_noise_model.hpp"
#include "rfmodel/linear/frozen_small_signal.hpp"
#include "rfmodel/noise/loaded_noise.hpp"
#include "rfmodel/noise/noise_renormalization.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
constexpr double weak = 1e-16;

SMatrix zero(std::size_t n) {
    return {n, std::vector<Complex>(n * n)};
}

SMatrix identity(std::size_t n) {
    auto result = zero(n);
    for (std::size_t i = 0; i < n; ++i) {
        result(i, i) = 1.;
    }
    return result;
}

SMatrix diagonal(double first, double second) {
    return {2, {first, 0., 0., second}};
}

// Every nonzero entry supplies its own scale. A unit/global absolute tolerance
// would let all of these weak-component migration regressions pass with zero.
void relative(Complex actual, Complex expected, const char *message) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()), message);
    if (expected == Complex{}) {
        require(actual == Complex{}, message);
    } else {
        require(std::abs(actual / expected - 1.) < 3e-12, message);
    }
}

void compare(const SMatrix &actual, const SMatrix &expected, const char *message) {
    require(actual.ports == expected.ports && actual.values.size() == expected.values.size(),
            message);
    for (std::size_t i = 0; i < expected.values.size(); ++i) {
        relative(actual.values[i], expected.values[i], message);
    }
}

TouchstoneData scattering_table(std::size_t n) {
    TouchstoneData data;
    data.ports = n;
    data.frequencies_hz = {1., 3.};
    data.matrices = {zero(n), zero(n)};
    return data;
}

void loaded_paths() {
    const SMatrix s = diagonal(.25, .5);
    const NoiseCorrelation c{diagonal(1., weak)}, e{diagonal(.5, 2. * weak)};
    const std::vector<Complex> gamma{.2, -.3};
    const auto result = loaded_noise(s, c, gamma, e);
    auto expected_in = zero(2), expected_out = zero(2);
    for (std::size_t i = 0; i < 2; ++i) {
        const Complex h = 1. / (1. - s(i, i) * gamma[i]);
        expected_out(i, i) =
            std::norm(h) * (c.watts_per_hz(i, i) + std::norm(s(i, i)) * e.watts_per_hz(i, i));
        expected_in(i, i) =
            std::norm(h) * (std::norm(gamma[i]) * c.watts_per_hz(i, i) + e.watts_per_hz(i, i));
        relative(result.net_into_device_w_per_hz[i],
                 expected_in(i, i) - expected_out(i, i),
                 "loaded weak net density");
    }
    compare(result.incident.watts_per_hz, expected_in, "loaded weak incident density");
    compare(result.outgoing.watts_per_hz, expected_out, "loaded weak outgoing density");
}

void reference_paths() {
    const auto s = zero(2);
    const NoiseCorrelation q{diagonal(1., weak)};
    for (double reference : {50., 75.}) {
        const double gain_squared = 4. * 50. * reference / ((50. + reference) * (50. + reference));
        compare(renormalize_noise(s, q, 50., reference).watts_per_hz,
                diagonal(gain_squared, gain_squared * weak),
                "common-real identity/changed reference weak density");
    }
    for (const std::vector<Complex> &old_refs :
         {std::vector<Complex>{50., 75.}, std::vector<Complex>{{50., 20.}, {75., -10.}}}) {
        for (const auto &new_refs : {old_refs, std::vector<Complex>{{30., -15.}, {120., 25.}}}) {
            auto expected = zero(2);
            for (std::size_t i = 0; i < 2; ++i) {
                // With S_old=0 and a_new=0, solving V/I gives this scalar transfer.
                const double gain_squared = 4. * old_refs[i].real() * new_refs[i].real() /
                                            std::norm(new_refs[i] + std::conj(old_refs[i]));
                expected(i, i) = gain_squared * q.watts_per_hz(i, i);
            }
            compare(renormalize_noise(s, q, old_refs, new_refs).watts_per_hz,
                    expected,
                    "vector real/complex identity/changed reference weak density");
        }
    }
}

void table_paths() {
    NoiseTable samples{{1., 3.}, {{diagonal(1., weak)}, {diagonal(2., 3. * weak)}}};
    const TabulatedNoiseModel model("independent scales", scattering_table(2), samples);
    samples.samples.front().watts_per_hz(1, 1) = 99.;
    compare(model.noise_correlation(1.).watts_per_hz,
            diagonal(1., weak),
            "table weak sample retained and owned");
    compare(model.noise_correlation(2.).watts_per_hz,
            diagonal(1.5, 2. * weak),
            "table weak interpolation retained");
    const auto repeated = renormalize_noise(zero(2), model.noise_correlation(2.), 50., 50.);
    compare(repeated.watts_per_hz, diagonal(1.5, 2. * weak), "table weak repeated projection");
}

void conversion_paths() {
    const SMatrix a{2, {1., 0., 0., Complex{2., 1.}}};
    const SMatrix b{2, {0., 0., 0., Complex{.25, -.5}}};
    const FrequencyConversionModel model(1., {{0, 1}, {1, 2}}, a, b);
    const ConversionNoise source{diagonal(1., weak),
                                 SMatrix{2, {.25, 0., 0., Complex{.2, .3} * weak}}};
    auto intrinsic = source;
    for (auto *matrix : {&intrinsic.covariance, &intrinsic.complementary}) {
        for (auto &value : matrix->values) {
            value *= .5;
        }
    }
    model.validate_noise(source);
    const auto result = model.analyze({0., 0.}, {0., 0.}, source, intrinsic, {}, true);
    auto expected_c = zero(2), expected_p = zero(2);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto direct = a(i, i), conjugate = b(i, i);
        const auto c = source.covariance(i, i), p = source.complementary(i, i);
        // Direct scalar moments of z'=a*z+b*conj(z), independent of quadrature helpers.
        expected_c(i, i) = (std::norm(direct) + std::norm(conjugate)) * c +
                           2. * std::real(direct * std::conj(conjugate) * p) + .5 * c;
        expected_p(i, i) = direct * direct * p + conjugate * conjugate * std::conj(p) +
                           2. * direct * conjugate * c + .5 * p;
    }
    compare(result.outgoing_noise.covariance, expected_c, "conversion weak C retained");
    compare(result.outgoing_noise.complementary, expected_p, "conversion weak P retained");
    compare(result.incident_noise.covariance, source.covariance, "conversion weak incident C");
    compare(
        result.incident_noise.complementary, source.complementary, "conversion weak incident P");

    const FrequencyConversionModel dc(1., {{0, 0}, {1, 0}}, identity(2), zero(2));
    const ConversionNoise real{diagonal(1., weak), diagonal(1., weak)};
    const auto real_result = dc.analyze({0., 0.}, {0., 0.}, real, dc.zero_noise(), {}, true);
    compare(real_result.outgoing_noise.covariance, real.covariance, "real DC weak C");
    compare(real_result.outgoing_noise.complementary, real.complementary, "real DC weak P");
    require(real_result.outgoing_noise.covariance.values ==
                real_result.outgoing_noise.complementary.values,
            "real DC C must equal P exactly");
    auto nonreal = real;
    nonreal.complementary(1, 1) = 0.;
    rejects<std::invalid_argument>([&] {
        dc.validate_noise(nonreal);
    });

    // Tiny improper noise is globally admitted as before; failed local PSD
    // admission must not turn the recovery attempt into a new rejection.
    const FrequencyConversionModel identity_model(1., {{0, 1}, {1, 2}}, identity(2), zero(2));
    auto invalid_weak = source;
    invalid_weak.complementary(1, 1) = Complex{2., .5} * weak;
    rejects<std::invalid_argument>([&] {
        identity_model.validate_noise({diagonal(1., -1.), zero(2)});
    });
    const auto fallback =
        identity_model.analyze({0., 0.}, {0., 0.}, invalid_weak, identity_model.zero_noise());
    relative(fallback.outgoing_noise.covariance(1, 1), 0., "conversion local rejection C fallback");
    relative(
        fallback.outgoing_noise.complementary(1, 1), 0., "conversion local rejection P fallback");
}

ModelAdapter adapter(const SMatrix &q) {
    ModelFunctions functions;
    functions.scattering = [n = q.ports](double) {
        return zero(n);
    };
    functions.noise = [q](double) {
        return NoiseCorrelation{q};
    };
    ModelDescriptor descriptor{{"test.independent.noise", "1", "", ""}, "independent noise", {}};
    for (std::size_t i = 0; i < q.ports; ++i) {
        descriptor.ports.push_back({i, "port", 50.});
    }
    return ModelAdapter(std::move(descriptor), std::move(functions));
}

void adapter_validation() {
    for (const auto &q :
         {diagonal(1., weak), diagonal(1., -weak), SMatrix{2, {1., 2e-8, 2e-8, weak}}}) {
        const auto result = adapter(q).noise_correlation(1.).watts_per_hz;
        require(result.values == q.values,
                "adapter validation must not replace supplied covariance");
    }
    for (const auto &q : {diagonal(1., -1.), SMatrix{2, {1., .1, 0., weak}}}) {
        rejects<std::invalid_argument>([&] {
            adapter(q).noise_correlation(1.);
        });
    }
}

FrozenSmallSignal frozen_fixture() {
    DcDevice device;
    device.name = "two independent one-ohm resistors";
    device.terminals = {1, 2};
    device.evaluate = [](const std::vector<double> &v) {
        return DcDeviceEvaluation{v, {1., 0., 0., 1.}, std::nullopt};
    };
    return freeze_small_signal(solve_dc({2, {device}, {0., 0.}}, {0., 0.}));
}

void frozen_path() {
    const auto frozen = frozen_fixture();
    const NodalCurrentNoise q{diagonal(1., weak)};
    for (double f : {0., 1.}) {
        const std::vector<Complex> refs =
            f == 0. ? std::vector<Complex>{1., 2.} : std::vector<Complex>{{1., .5}, {2., -.25}};
        auto expected = zero(2);
        for (std::size_t i = 0; i < 2; ++i) {
            // V=j/(1+1/Z), b=sqrt(Re Z)*V/Z for these independent G=1 branches.
            expected(i, i) =
                q.ampere_squared_per_hz(i, i) * refs[i].real() / std::norm(1. + refs[i]);
        }
        compare(frozen.external_noise(f, {1, 2}, refs, q).watts_per_hz,
                expected,
                "frozen direct physical current weak block");
    }
}

void independent_assembly() {
    const SMatrix correlated{2, {weak, Complex{0., .5 * weak}, Complex{0., -.5 * weak}, weak}};
    const auto assembled = independent_noise({{{1, {1.}}}, {correlated}});
    auto expected = zero(3);
    expected(0, 0) = 1.;
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            expected(i + 1, j + 1) = correlated(i, j);
        }
    }
    compare(assembled.watts_per_hz, expected, "independent weak correlated assembly");
    compare(propagate_noise(identity(3), assembled).watts_per_hz,
            expected,
            "independent weak correlated block survives global refactor");
    compare(independent_noise({{diagonal(1., weak)}}).watts_per_hz,
            diagonal(1., weak),
            "independent block input uses recovery");
}

void projection_admission_boundaries() {
    // Public reconstruction callers still admit tiny negative/indefinite inputs,
    // and a connected, partly retained component keeps its old projection.
    const std::vector<SMatrix> inputs{diagonal(1., -weak), SMatrix{2, {1., 2e-8, 2e-8, weak}}};
    const std::vector<SMatrix> expected{diagonal(1., 0.), SMatrix{2, {1., 2e-8, 2e-8, 4e-16}}};
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        const auto &q = inputs[k];
        compare(renormalize_noise(zero(2), {q}, 50., 50.).watts_per_hz,
                expected[k],
                "common-real original admission/projection");
        compare(loaded_noise(zero(2), {q}, {0., 0.}, {zero(2)}).outgoing.watts_per_hz,
                expected[k],
                "loaded original admission/projection");
        const TabulatedNoiseModel table("legacy admission", scattering_table(2), {{1.}, {{q}}});
        compare(table.noise_correlation(1.).watts_per_hz,
                expected[k],
                "table original admission/projection");
    }
    const NoiseCorrelation bad{diagonal(1., -1.)};
    rejects<std::invalid_argument>([&] {
        loaded_noise(zero(2), bad, {0., 0.}, {zero(2)});
    });
    rejects<std::invalid_argument>([&] {
        renormalize_noise(zero(2), bad, 50., 50.);
    });
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel table("bad", scattering_table(2), {{1.}, {bad}});
    });
    rejects<std::invalid_argument>([&] {
        frozen_fixture().external_noise(1., {1, 2}, {1., 1.}, {bad.watts_per_hz});
    });

    // The passivity scale floor keeps a representable, roundoff-sized independent
    // loss discarded, even though an unfloored covariance would now recover it.
    const double almost_one = std::nextafter(1., 0.);
    const auto thermal = passive_thermal_noise(diagonal(0., almost_one), 300.);
    relative(thermal.watts_per_hz(0, 0), 1.380649e-23 * 300., "strong thermal density");
    relative(thermal.watts_per_hz(1, 1), 0., "passivity scale floor remains unchanged");
}
} // namespace

int main() {
    try {
        loaded_paths();
        reference_paths();
        table_paths();
        conversion_paths();
        adapter_validation();
        frozen_path();
        independent_assembly();
        projection_admission_boundaries();
        std::cout << "Independent weak noise caller regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
