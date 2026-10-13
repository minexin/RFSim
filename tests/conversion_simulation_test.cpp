#include "rfmodel/conversion_simulation.hpp"
#include "test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
void close_noise(Complex actual, Complex expected, double scale) {
    require(std::abs(actual - expected) <= 3e-11 * std::max(scale, std::abs(expected)),
            "independent C/P mismatch");
}

void grid_transport() {
    const std::vector<ConversionFrequencyDevice> devices{
        {{0, 1}, {{0, 2}, {0, 0}}, linear_frequency_rules({0, 1})},
        {{3, 4}, {{4, 3}}, linear_frequency_rules({3, 4})}};
    ConversionGrid grid(1e8, devices, {{0, 1, 1, 3}});
    require(grid.plan().total_channels == 12 &&
                grid.global_seed_indices() == std::vector<std::size_t>{1, 0, 11},
            "global seed mapping differs");
    ConversionSeedState seeds;
    seeds.source = {Complex{.2, .1}, .3};
    seeds.output_offset = {.1, .2};
    seeds.initial_incident = {.4, .5};
    seeds.references = {Complex{31., 12.}, 75.};
    seeds.selected_output_channels = {1, 0};
    ConversionNoise noise{conversion_detail::zero(2), conversion_detail::zero(2)};
    // One real latent source, retaining nontrivial RF/DC complementary noise.
    const std::vector<Complex> basis{Complex{1., 2.}, .5};
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            noise.covariance(i, j) = 1e-18 * basis[i] * std::conj(basis[j]);
            noise.complementary(i, j) = 1e-18 * basis[i] * basis[j];
        }
    }
    seeds.source_noise = noise;
    const auto expanded = grid.expand(0, seeds, 50., GeneratedConversionBoundary{.2, {}, 290.});
    near(expanded.source[0], .3);
    near(expanded.source[1], Complex{.2, .1});
    near(expanded.output_offset[0], .2);
    near(expanded.initial_incident[1], .4);
    near(expanded.references[1], Complex{31., 12.});
    near(expanded.references[0], 75.);
    require(expanded.selected_output_channels == std::vector<std::size_t>{0, 1},
            "AM/PM indices were not remapped");
    near(expanded.reflection[2], .2);
    near(expanded.reflection[3], 0.); // A generated wired port must not get a boundary.
    close_noise(expanded.source_noise.covariance(2, 2), 1.380649e-23 * 290. * .96, 1e-21);
    close_noise(expanded.source_noise.covariance(1, 0), noise.covariance(0, 1), 1e-18);
    close_noise(expanded.source_noise.complementary(0, 1), noise.complementary(1, 0), 1e-18);
    const auto other = grid.expand(1, {}, 50., GeneratedConversionBoundary{0., {}, 290.});
    close_noise(other.source_noise.complementary(3, 3), 1.380649e-23 * 290., 1e-21);
    require(grid.generated_external_channels().size() == 3, "generated external identity count");
    ConversionNoise global{conversion_detail::zero(3), conversion_detail::zero(3)};
    const std::vector<Complex> latent{Complex{1., 2.}, .5, Complex{-.3, .8}};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            global.covariance(i, j) = 1e-18 * latent[i] * std::conj(latent[j]);
            global.complementary(i, j) = 1e-18 * latent[i] * latent[j];
        }
    }
    const auto joint = grid.global_noise(global);
    close_noise(joint.covariance(1, 11), global.covariance(0, 2), 1e-18);
    close_noise(joint.complementary(11, 0), global.complementary(2, 1), 1e-18);
    auto bad = seeds;
    bad.source = {.1};
    rejects<std::invalid_argument>([&] {
        grid.expand(0, bad);
    });
    bad = seeds;
    bad.references[1] = Complex{50., 1.};
    rejects<std::invalid_argument>([&] {
        grid.expand(0, bad);
    });
    rejects<std::invalid_argument>([&] {
        grid.local_indices(0, {1, 1});
    });

    // Parameterized linear models commonly arrive in bin-major order.
    const std::vector<ConversionChannel> bin_major{{0, 0}, {1, 0}, {0, 2}, {1, 2}, {0, 3}, {1, 3}};
    auto direct = conversion_detail::zero(6);
    for (std::size_t i = 0; i < 6; ++i) {
        direct(i, i) = .01 * (i + 1);
    }
    FrequencyConversionModel model(1e8, bin_major, direct, conversion_detail::zero(6));
    ConversionDevice device{model,
                            {0., 1., 2., 3., 4., 5.},
                            std::vector<Complex>(6),
                            model.zero_noise(),
                            model.zero_noise()};
    const auto aligned = grid.align_device(0, device);
    const std::vector<std::size_t> permutation{0, 2, 4, 1, 3, 5};
    for (std::size_t i = 0; i < 6; ++i) {
        near(aligned.source[i], static_cast<double>(permutation[i]));
        near(aligned.model.direct()(i, i), .01 * (permutation[i] + 1));
    }
    require(grid.align_vector(0, bin_major, device.source) == aligned.source,
            "offset alignment differs");
}

ConversionDevice pad(const std::vector<Complex> &refs, Complex carrier, double temperature) {
    const std::vector<ConversionChannel> channels{
        {0, 9}, {1, 9}, {0, 10}, {1, 10}, {0, 11}, {1, 11}};
    auto direct = conversion_detail::zero(6);
    for (std::size_t i = 0; i < 6; i += 2) {
        direct(i, i + 1) = direct(i + 1, i) = .5;
    }
    FrequencyConversionModel original(1e8, channels, direct, conversion_detail::zero(6));
    auto intrinsic = original.zero_noise();
    for (std::size_t i = 0; i < 6; ++i) {
        intrinsic.covariance(i, i) = 1.380649e-23 * temperature * .75;
    }
    const auto change = renormalize_power_waves(original, refs);
    const std::vector<Complex> source{0., 0., carrier, 0., 0., 0.};
    const auto boundary = renormalize_conversion_boundary(
        original, refs, {0, 1, 2, 3, 4, 5}, source, std::vector<Complex>(6), original.zero_noise());
    return {change.model,
            boundary.source,
            boundary.reflection,
            boundary.source_noise,
            renormalize_conversion_noise(change, intrinsic)};
}

void phase_ensemble(bool shared, double scale) {
    std::vector<Complex> refs{
        {31., 12.}, {73., -21.}, {19., 8.}, {110., 31.}, {22., -7.}, {91., -18.}};
    for (auto &reference : refs) {
        reference *= scale;
    }
    const std::vector<Complex> carriers{Complex{.01, .005}, Complex{-.004, .009}};
    ConversionSimulationRequest request;
    request.devices = {pad(refs, carriers[0], 290.), pad(refs, carriers[1], 290.)};
    request.loaded_noise = true;
    if (shared) {
        request.phase_noise = {{"clock", {{{0, 0, 10}, 1.}, {{1, 0, 10}, -.7}}, {{1, -90.}}}};
    } else {
        request.phase_noise = {{"first", {{{0, 0, 10}, 1.}}, {{1, -90.}}},
                               {"second", {{{1, 0, 10}, -.7}}, {{1, -90.}}}};
    }
    request.measurements = {
        {"output-band", 0, 1, 1e9, 2e8, {10}, ConversionWaveDirection::outgoing}};
    request.noise_experiments = {{"pad-NF", {{0, 0, 10}}, {{0, 0, 10}}, {0, 1, 10}, 290.}};
    const auto result = analyze_conversion_simulation(request);
    const auto n = std::size_t{12};
    auto caa = conversion_detail::zero(n), paa = caa, cbb = caa, pbb = caa, cab = caa, pab = caa;
    // Independent V/I transformation of four proper-random phase samples. The
    // thermal latent sources are added analytically below, one per physical port.
    const std::vector<Complex> phases{std::sqrt(1e-9),
                                      Complex{0., std::sqrt(1e-9)},
                                      -std::sqrt(1e-9),
                                      Complex{0., -std::sqrt(1e-9)}};
    for (std::size_t group = 0; group < (shared ? 1u : 2u); ++group) {
        for (auto phase : phases) {
            std::vector<Complex> a(n), b(n);
            for (std::size_t d = 0; d < 2; ++d) {
                if (!shared && d != group) {
                    continue;
                }
                const double gain = d ? -.7 : 1.;
                for (auto bin : {9, 11}) {
                    const auto emission = Complex{0., 1.} * carriers[d] * gain *
                                          (bin == 9 ? std::conj(phase) : phase);
                    for (std::size_t port = 0; port < 2; ++port) {
                        const auto local = static_cast<std::size_t>(bin - 9) * 2 + port;
                        const auto index = 6 * d + local;
                        const auto common_a = port == 0 ? emission : Complex{};
                        const auto common_b = port == 1 ? .5 * emission : Complex{};
                        const auto voltage = std::sqrt(50.) * (common_a + common_b);
                        const auto current = (common_a - common_b) / std::sqrt(50.);
                        const auto z = refs[local];
                        a[index] = (voltage + z * current) / (2. * std::sqrt(z.real()));
                        b[index] = (voltage - std::conj(z) * current) / (2. * std::sqrt(z.real()));
                    }
                }
            }
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    caa(i, j) += a[i] * std::conj(a[j]) / 4.;
                    paa(i, j) += a[i] * a[j] / 4.;
                    cbb(i, j) += b[i] * std::conj(b[j]) / 4.;
                    pbb(i, j) += b[i] * b[j] / 4.;
                    cab(i, j) += a[i] * std::conj(b[j]) / 4.;
                    pab(i, j) += a[i] * b[j] / 4.;
                }
            }
        }
    }
    const double density = 1.380649e-23 * 290. * .75;
    for (std::size_t i = 0; i < n; ++i) {
        const auto z = refs[i % 6];
        const auto beta = (50. - z) / (2. * std::sqrt(50. * z.real()));
        const auto delta = (50. + std::conj(z)) / (2. * std::sqrt(50. * z.real()));
        caa(i, i) += density * std::norm(beta);
        cbb(i, i) += density * std::norm(delta);
        cab(i, i) += density * beta * std::conj(delta);
    }
    const auto &actual = result.waves;
    for (std::size_t i = 0; i < n * n; ++i) {
        close_noise(actual.incident_noise.covariance.values[i], caa.values[i], 1e-18);
        close_noise(actual.incident_noise.complementary.values[i], paa.values[i], 1e-18);
        close_noise(actual.outgoing_noise.covariance.values[i], cbb.values[i], 1e-18);
        close_noise(actual.outgoing_noise.complementary.values[i], pbb.values[i], 1e-18);
        close_noise(actual.incident_outgoing_noise.covariance.values[i], cab.values[i], 1e-18);
        close_noise(actual.incident_outgoing_noise.complementary.values[i], pab.values[i], 1e-18);
    }
    near(result.noise_experiments.front().second.noise_factor, 4.);
    near(result.channels[3].voltage_v, std::sqrt(50.) * .5 * carriers[0]);
    near(result.channels[3].current_a, -.5 * carriers[0] / std::sqrt(50.));
    near(result.channels[3].net_into_device_w, -.25 * std::norm(carriers[0]));
    const double integrated =
        1e8 * (cbb(1, 1).real() + 2. * cbb(3, 3).real() + cbb(5, 5).real()) / 2.;
    close_noise(result.measurements.front().second.noise_power_w, integrated, 1e-12);
    require(result.additional_source_noise.has_value(),
            "phase contribution was not retained in snapshot");
    auto invalid = request;
    invalid.phase_noise.front().members.front().carrier.bin = 8;
    rejects<std::invalid_argument>([&] {
        analyze_conversion_simulation(invalid);
    });
}

void accepted_nonlinear_snapshot() {
    FrequencyConversionModel model(1., {{0, 1}}, {1, {0.}}, {1, {0.}});
    auto source_noise = model.zero_noise();
    source_noise.covariance(0, 0) = 1e-9;
    ConversionSimulationRequest request;
    request.devices = {{model, {1.}, {1.}, source_noise, model.zero_noise()}};
    request.operating_point = ConversionOperatingOptions{};
    request.initial_incident = {.01};
    request.loaded_noise = true;
    std::size_t calls = 0;
    request.nonlinear = {
        {0, [&](const std::vector<Complex> &a) {
             ++calls;
             return ConversionLinearization{
                 FrequencyConversionModel(
                     1., {{0, 1}}, {1, {-2. * std::norm(a[0])}}, {1, {-a[0] * a[0]}}),
                 {-std::norm(a[0]) * a[0]}};
         }}};
    const auto result = analyze_conversion_simulation(request);
    const double a = result.waves.incident[0].real();
    require(std::abs(a + a * a * a - 1.) < 1e-9, "nonlinear physical residual differs");
    near(result.waves.outgoing[0], -a * a * a);
    near(result.accepted_devices[0].model.direct()(0, 0), -2. * a * a);
    near(result.accepted_devices[0].model.conjugate()(0, 0), -a * a);
    near(result.output_offset[0], 2. * a * a * a);
    const double radial = -3. * a * a / (1. + 3. * a * a), tangent = -a * a / (1. + a * a);
    close_noise(result.waves.outgoing_noise.covariance(0, 0),
                1e-9 * .5 * (radial * radial + tangent * tangent),
                1e-9);
    close_noise(result.waves.outgoing_noise.complementary(0, 0),
                1e-9 * .5 * (radial * radial - tangent * tangent),
                1e-9);
    const auto before = calls;
    FrequencyConversionNetwork accepted(1.);
    accepted.add_power_waves(result.accepted_devices.front());
    const auto replay = accepted.analyze(true, nullptr, result.output_offset);
    near(replay.outgoing[0], result.waves.outgoing[0]);
    require(calls == before && result.operating_point->iterations > 1,
            "post-processing reran nonlinear callback");
}
} // namespace

int main() {
    try {
        grid_transport();
        for (bool shared : {false, true}) {
            for (double scale : {.5, 1., 2.}) {
                phase_ensemble(shared, scale);
            }
        }
        accepted_nonlinear_snapshot();
        std::cout << "Typed conversion simulation: seed transport, full physical C/P, measurements "
                     "and accepted nonlinear snapshot passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
