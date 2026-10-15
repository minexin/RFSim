#include "frozen_model.hpp"
#include <rfmodel/linear/frozen_small_signal.hpp>
#include <rfmodel/linear/linear_analysis.hpp>
#include <array>
#include <cmath>
#include <stdexcept>

namespace frozen_example {
using namespace rfmodel;

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(Complex actual, Complex expected, double scale = 1.) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()) &&
                std::abs(actual - expected) <= 2e-11 * scale,
            "independent frozen RC oracle mismatch");
}

// Independent 2x2 determinant calculation, not a call to the SDK matrix solver.
struct Oracle {
    std::array<Complex, 4> s, noise;
};

Oracle oracle(double hz) {
    const double w = 2. * std::acos(-1.) * hz;
    const Complex a{.05, w * 1e-9}, d{.04, w * 2e-9}, b = -.01;
    const Complex determinant = a * d - b * b;
    const std::array<Complex, 4> inverse{
        d / determinant, -b / determinant, -b / determinant, a / determinant};
    Oracle result;
    for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t c = 0; c < 2; ++c) {
            result.s[2 * r + c] = .04 * inverse[2 * r + c] - (r == c ? 1. : 0.);
            result.noise[2 * r + c] = (inverse[2 * r] * std::conj(inverse[2 * c]) * 1e-20 +
                                       inverse[2 * r + 1] * std::conj(inverse[2 * c + 1]) * 2e-20) /
                                      50.;
        }
    }
    return result;
}

void matrices(const SMatrix &s, const NoiseCorrelation &noise, double hz) {
    require(s.ports == 2 && noise.watts_per_hz.ports == 2, "wrong matrix dimensions");
    const auto expected = oracle(hz);
    for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t c = 0; c < 2; ++c) {
            near(s(r, c), expected.s[2 * r + c]);
            near(noise.watts_per_hz(r, c), expected.noise[2 * r + c], 1e-18);
        }
    }
}
} // namespace

ModelAdapter make_model(Diagnostics &diagnostics, Noise noise) {
    // The lifetime token is owned solely by the source device callback.
    auto token = std::make_shared<const int>(1);
    diagnostics.dc_callback_lifetime = token;
    DcDevice device{"RC", {1, 2}, [&diagnostics, token](const std::vector<double> &v) {
                        diagnostics.dc_evaluations += *token;
                        return DcDeviceEvaluation{
                            {.03 * v[0] - .01 * v[1], -.01 * v[0] + .02 * v[1]},
                            {.03, -.01, -.01, .02},
                            DcChargeEvaluation{{1e-9 * v[0], 2e-9 * v[1]}, {1e-9, 0, 0, 2e-9}}};
                    }};
    auto frozen = freeze_small_signal(solve_dc({2, {device}, {.02, 0}}, {0, 0}));
    near(frozen.operating_point().voltage_volt()[0], .8);
    near(frozen.operating_point().voltage_volt()[1], .4);
    const std::vector<DcNode> nodes{1, 2};
    const std::vector<Complex> references{50., 50.};
    ModelFunctions functions;
    functions.scattering = [frozen, nodes, references](double hz) {
        return frozen.external_s(hz, nodes, references);
    };
    if (noise != Noise::missing) {
        const double q = noise == Noise::supplied ? 1e-20 : 0.;
        functions.noise = [frozen,
                           nodes,
                           references,
                           current_noise = NodalCurrentNoise{{2, {q, 0, 0, 2 * q}}}](double hz) {
            return frozen.external_noise(hz, nodes, references, current_noise);
        };
    }
    return {
        {{"example.frozen-rc", "1", "", ""}, "Frozen RC", {{0, "node 1", 50.}, {1, "node 2", 50.}}},
        std::move(functions)};
}

void verify_workflows() {
    Diagnostics diagnostics, missing_diagnostics, zero_diagnostics;
    auto model = make_model(diagnostics, Noise::supplied);
    auto missing = make_model(missing_diagnostics, Noise::missing);
    auto zero = make_model(zero_diagnostics, Noise::explicit_zero);
    require(diagnostics.dc_callback_lifetime.expired() &&
                missing_diagnostics.dc_callback_lifetime.expired() &&
                zero_diagnostics.dc_callback_lifetime.expired(),
            "source callbacks retained");
    const int evaluations = diagnostics.dc_evaluations;
    require(evaluations > 0, "DC callback not exercised");
    const auto capabilities = model.capabilities();
    require(capabilities.linear && capabilities.noise && !capabilities.operating_point &&
                !capabilities.spectrum && !capabilities.coherent,
            "incorrect capabilities");
    require(!missing.capabilities().noise && zero.capabilities().noise,
            "missing noise is not zero");
    const std::vector<double> frequencies{0., 1e6, 1e7};
    for (double hz : frequencies) {
        matrices(model.s_parameters(hz), model.noise_correlation(hz), hz);
    }
    auto builder = [model](double hz) {
        LinearNetwork network(50.);
        model.add_to(network, hz);
        return network;
    };
    // This network contains exactly one block. A multiblock network needs a full
    // global covariance, not this single-device matrix reused as network noise.
    const auto analysis =
        analyze_linear({frequencies}, {0, 1}, builder, [model](double hz, const LinearNetwork &) {
            return model.noise_correlation(hz);
        });
    require(analysis.scattering.size() == 3 && analysis.noise_correlation.size() == 3,
            "incomplete linear analysis");
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        matrices(analysis.scattering[k], analysis.noise_correlation[k], frequencies[k]);
        require(analysis.port_impedances_ohms[k] == std::vector<Complex>({50., 50.}),
                "reference coordinates changed");
    }
    bool rejected = false;
    try {
        missing.noise_correlation(1e6);
    } catch (const ModelCapabilityUnavailable &) {
        rejected = true;
    }
    require(rejected, "missing noise accepted");
    rejected = false;
    try {
        missing.conversion_device(1e6, {0, 1, 10}, ModelNoisePolicy::require_model_noise);
    } catch (const ModelCapabilityUnavailable &) {
        rejected = true;
    }
    require(rejected, "conversion accepted missing required noise");
    const auto omitted =
        missing.conversion_device(1e6, {0, 1, 10}, ModelNoisePolicy::omit_intrinsic_noise);
    const auto explicit_zero =
        zero.conversion_device(1e6, {0, 1, 10}, ModelNoisePolicy::require_model_noise);
    // Returned snapshot outlives both its adapter and its DC source callbacks.
    auto snapshot = [] {
        Diagnostics local;
        return make_model(local, Noise::supplied)
            .conversion_device(1e6, {0, 1, 10}, ModelNoisePolicy::require_model_noise);
    }();
    require(snapshot.model.channels().size() == 6, "conversion channel count");
    for (std::size_t r = 0; r < 6; ++r) {
        require(snapshot.model.channels()[r].port == r % 2 &&
                    snapshot.model.channels()[r].bin == std::array<int, 3>{0, 1, 10}[r / 2],
                "conversion channel ordering");
        for (std::size_t c = 0; c < 6; ++c) {
            const auto expected = oracle(frequencies[r / 2]);
            const auto s = r / 2 == c / 2 ? expected.s[2 * (r % 2) + c % 2] : Complex{};
            const auto n = r / 2 == c / 2 ? expected.noise[2 * (r % 2) + c % 2] : Complex{};
            near(snapshot.model.direct()(r, c), s);
            near(snapshot.model.conjugate()(r, c), 0.);
            near(snapshot.intrinsic_noise.covariance(r, c), n, 1e-18);
            near(snapshot.intrinsic_noise.complementary(r, c),
                 r < 2 && c < 2 ? n : Complex{},
                 1e-18);
            near(omitted.intrinsic_noise.covariance(r, c), 0., 1e-18);
            near(explicit_zero.intrinsic_noise.covariance(r, c), 0., 1e-18);
        }
    }
    snapshot.source[2] = {.2, .1};
    FrequencyConversionNetwork conversion(1e6, 50.);
    conversion.add_power_waves(snapshot);
    const auto converted = conversion.analyze();
    near(converted.outgoing[3], oracle(1e6).s[2] * Complex{.2, .1});
    for (std::size_t r = 0; r < 6; ++r) {
        for (std::size_t c = 0; c < 6; ++c) {
            near(converted.outgoing_noise.covariance(r, c),
                 snapshot.intrinsic_noise.covariance(r, c),
                 1e-18);
            near(converted.outgoing_noise.complementary(r, c),
                 snapshot.intrinsic_noise.complementary(r, c),
                 1e-18);
        }
    }
    const PowerWaveSpectrum input{1e6, {{1, {.2, .1}}, {10, {.3, -.1}}}};
    const auto spectrum = transmit_linear_spectrum(input, {0, 1}, 50., builder);
    require(spectrum.spacing_hz == input.spacing_hz && spectrum.amplitudes.size() == 2,
            "spectrum grid changed");
    for (const auto &entry : input.amplitudes) {
        near(spectrum.amplitudes.at(entry.first), entry.second * oracle(entry.first * 1e6).s[2]);
    }
    const auto plan = [builder] {
        CoherentSystem graph(1e6, 50., {{"rf", "clock"}});
        auto source =
            graph.input("rf", {{1, SpectrumKind::source, 1., graph.source_group("rf"), .2}});
        graph.linear_network("RC", {{source, 0}}, {0, 1}, {{"out", 1}}, builder);
        return graph.compile();
    }();
    const auto coherent = plan.execute();
    near(coherent.stream("out").reduction.total_power_w, .04 * std::norm(oracle(1e6).s[2]));
    require(diagnostics.dc_evaluations == evaluations, "frozen evaluation called DC source");
}
} // namespace frozen_example
