#include "rfmodel/noise/phase_response.hpp"
#include "rfmodel/noise/phase_noise.hpp"
#include "rfmodel/conversion/conversion_phase_response.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <iostream>

using namespace rfmodel;

namespace {
const Complex I{0., 1.};
const std::array<Complex, 4> phases{{1., I, -1., -I}};
using Samples = std::vector<std::vector<Complex>>;

void close(Complex actual, Complex expected, double scale = 1e-10) {
    require(std::abs(actual - expected) <= 3e-12 * std::max(scale, std::abs(expected)),
            "phase response C/P oracle mismatch");
}

ConversionNoise moments(const Samples &samples) {
    const auto n = samples.front().size();
    ConversionNoise q{conversion_detail::zero(n), conversion_detail::zero(n)};
    for (const auto &z : samples) {
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                q.covariance(i, j) += z[i] * std::conj(z[j]) / double(samples.size());
                q.complementary(i, j) += z[i] * z[j] / double(samples.size());
            }
        }
    }
    return q;
}

void compare(const ConversionNoise &a, const ConversionNoise &b, double scale = 1e-10) {
    require(a.covariance.values.size() == b.covariance.values.size(), "noise shape mismatch");
    for (std::size_t i = 0; i < a.covariance.values.size(); ++i) {
        close(a.covariance.values[i], b.covariance.values[i], scale);
        close(a.complementary.values[i], b.complementary.values[i], scale);
    }
}

ConversionNoise sum(ConversionNoise a, const ConversionNoise &b) {
    for (std::size_t i = 0; i < a.covariance.values.size(); ++i) {
        a.covariance.values[i] += b.covariance.values[i];
        a.complementary.values[i] += b.complementary.values[i];
    }
    return a;
}

// Independent time/Fourier samples: no production coefficient builder is used.
Samples
samples(const std::array<Complex, 2> &a, const std::array<Complex, 2> &h, bool independent) {
    Samples result;
    for (auto u : phases) {
        for (auto v : phases) {
            if (!independent && v != u) {
                continue;
            }
            std::vector<Complex> z(6);
            const std::array<Complex, 2> latent{{u, v}};
            for (std::size_t k = 0; k < 2; ++k) {
                const auto phi = h[k] * latent[k] * 1e-5;
                z[3 * k] = I * a[k] * std::conj(phi);
                z[3 * k + 2] = I * a[k] * phi;
            }
            result.push_back(z);
        }
    }
    return result;
}

const std::vector<ConversionChannel> channels{{0, 9}, {0, 10}, {0, 11}, {1, 9}, {1, 10}, {1, 11}};

void statistics_and_transport() {
    const std::array<Complex, 2> a{{{1., .3}, {.7, -.2}}}, h{{1., I}};
    const std::vector<PhaseResponseCarrier> carriers{{1, a[0]}, {4, a[1]}};
    const auto q = phase_response_group(channels, carriers, 2., {{1, 2., 2e-10, {h[0], h[1]}}});
    const auto shared = samples(a, h, false);
    compare(q, moments(shared));
    const auto independent =
        sum(phase_response_group(channels, {{1, a[0]}}, 2., {{1, 2., 2e-10, {h[0]}}}),
            phase_response_group(channels, {{4, a[1]}}, 2., {{1, 2., 2e-10, {h[1]}}}));
    compare(independent, moments(samples(a, h, true)));
    close(independent.covariance(2, 5), 0.);
    close(independent.complementary(2, 3), 0.);
    close(independent.complementary(5, 3), -a[1] * a[1] * 1e-10);
    close(q.complementary(2, 2), 0.);
    const auto signs =
        phase_response_group(channels, {{1, 1.}, {4, 1.}}, 2., {{1, 2., 2e-10, {1., I}}});
    close(signs.covariance(2, 5), -I * 1e-10);
    close(signs.covariance(0, 3), I * 1e-10);
    close(signs.complementary(2, 3), I * 1e-10);
    close(signs.complementary(5, 0), -I * 1e-10);
    auto A = conversion_detail::zero(6), B = A;
    for (std::size_t i = 0; i < 6; ++i) {
        A(i, i) = {.4, .1};
        B(i, i) = {-.05, .2};
        A(i, (i + 1) % 6) = {.03, -.02};
        B(i, (i + 2) % 6) = {.01, .015};
    }
    FrequencyConversionModel model(2., channels, A, B);
    model.validate_noise(q);
    model.validate_noise(independent);
    FrequencyConversionNetwork network(2.);
    network.add(model);
    const auto result = network.analyze(true, &q);
    Samples output;
    for (const auto &z : shared) {
        std::vector<Complex> y(6);
        for (std::size_t i = 0; i < 6; ++i) {
            for (std::size_t j = 0; j < 6; ++j) {
                y[i] += A(i, j) * z[j] + B(i, j) * std::conj(z[j]);
            }
        }
        output.push_back(y);
    }
    compare(result.outgoing_noise, moments(output));
    compare(result.incident_noise, q);
    const std::vector<Complex> reflection(6, Complex{.15, -.07});
    const auto loaded =
        model.analyze(std::vector<Complex>(6), reflection, q, model.zero_noise(), {}, true);
    Samples incident, outgoing;
    for (const auto &z : shared) {
        const auto deterministic = model.solve_waves(z, reflection);
        incident.push_back(deterministic.incident);
        outgoing.push_back(deterministic.outgoing);
    }
    compare(loaded.incident_noise, moments(incident));
    compare(loaded.outgoing_noise, moments(outgoing));
    for (std::size_t i = 0; i < 6; ++i) {
        for (std::size_t j = 0; j < 6; ++j) {
            Complex c{}, p{};
            for (std::size_t k = 0; k < shared.size(); ++k) {
                c += incident[k][i] * std::conj(outgoing[k][j]) / double(shared.size());
                p += incident[k][i] * outgoing[k][j] / double(shared.size());
            }
            close(loaded.incident_outgoing_noise.covariance(i, j), c);
            close(loaded.incident_outgoing_noise.complementary(i, j), p);
        }
    }
    auto invalid = model.zero_noise();
    for (std::size_t i = 0; i < 6; ++i) {
        invalid.covariance(i, i) = 1.;
        invalid.complementary(i, i) = 2.;
    }
    rejects<std::invalid_argument>([&] {
        model.validate_noise(invalid);
    });
    // Identity-based mapping remains correct when storage order changes.
    const std::array<std::size_t, 6> perm{{5, 1, 3, 2, 0, 4}};
    std::vector<ConversionChannel> shuffled;
    for (auto index : perm) {
        shuffled.push_back(channels[index]);
    }
    auto reordered =
        phase_response_group(shuffled, {{1, a[0]}, {5, a[1]}}, 2., {{1, 2., 2e-10, {h[0], h[1]}}});
    for (std::size_t i = 0; i < 6; ++i) {
        for (std::size_t j = 0; j < 6; ++j) {
            close(reordered.covariance(i, j), q.covariance(perm[i], perm[j]));
            close(reordered.complementary(i, j), q.complementary(perm[i], perm[j]));
        }
    }
}

void phase_scan_collision_and_offsets() {
    for (double theta : {0., .4, 1.5707963267948966, 3.141592653589793}) {
        const auto H = std::polar(1., theta);
        const auto q =
            phase_response_group(channels, {{1, 1.}, {4, 1.}}, 1., {{1, 1., 2e-10, {1., H}}});
        for (const auto &pair : {std::pair<std::size_t, std::size_t>{0, 3}, {2, 5}}) {
            const auto i = pair.first, j = pair.second;
            close((q.covariance(i, i) + q.covariance(j, j) - q.covariance(i, j) -
                   q.covariance(j, i)) /
                      2.,
                  1e-10 * (1. - std::cos(theta)));
        }
        const auto qi = sum(phase_response_group(channels, {{1, 1.}}, 1., {{1, 1., 2e-10, {1.}}}),
                            phase_response_group(channels, {{4, 1.}}, 1., {{1, 1., 2e-10, {H}}}));
        close((qi.covariance(2, 2) + qi.covariance(5, 5) - qi.covariance(2, 5) -
               qi.covariance(5, 2)) /
                  2.,
              1e-10);
    }
    const std::vector<ConversionChannel> collision{{0, 9}, {0, 10}, {0, 11}, {0, 12}, {0, 13}};
    const auto q =
        phase_response_group(collision, {{1, 1.}, {3, 1.}}, 1., {{1, 1., 2e-10, {1., I}}});
    Samples direct;
    for (auto u : phases) {
        direct.push_back({I * std::conj(u) * 1e-5,
                          0.,
                          (I * u + I * std::conj(I * u)) * 1e-5,
                          0.,
                          I * I * u * 1e-5});
    }
    compare(q, moments(direct));
    close(q.complementary(2, 2), I * 2e-10);
    // Two offsets are independent; all 16 combinations give an independent oracle.
    const std::vector<PhaseResponsePoint> points{{1, 3., 2e-10, {1.}}, {2, 6., 8e-10, {I}}};
    const auto mult = phase_response_group(collision, {{2, {1., .2}}}, 3., points);
    direct.clear();
    for (auto u : phases) {
        for (auto v : phases) {
            const Complex a{1., .2};
            direct.push_back({I * a * std::conj(I * v) * 2e-5,
                              I * a * std::conj(u) * 1e-5,
                              0.,
                              I * a * u * 1e-5,
                              I * a * I * v * 2e-5});
        }
    }
    compare(mult, moments(direct));
}

void real_equivalence_and_ownership() {
    std::vector<ConversionChannel> grid;
    for (std::size_t port = 0; port < 3; ++port) {
        for (int bin = 8; bin <= 12; ++bin) {
            grid.push_back({port, bin});
        }
    }
    std::vector<PhaseResponseCarrier> carriers{{2, {1., .3}}, {7, {.7, -.2}}, {12, 1.}};
    std::vector<PhaseResponsePoint> points{{1, 10., 2e-10, {1., -2., 0.}},
                                           {2, 20., 2e-12, {1., -2., 0.}}};
    const auto original = phase_response_group(grid, carriers, 10., points);
    compare(original,
            phase_noise_group(grid,
                              {{2, {1., .3}, 1.}, {7, {.7, -.2}, -2.}, {12, 1., 0.}},
                              {{1, -100.}, {2, -120.}}));
    auto copied = points;
    auto moved = std::move(copied);
    points[0].member_response[0] = 50.;
    points.clear();
    compare(original, phase_response_group(grid, carriers, 10., moved));
    carriers[0].wave = 10.;
    require(std::abs(original.covariance(3, 3).real() - 1.09e-10) < 1e-22,
            "result retained mutable input state");
}

void validation() {
    const std::vector<PhaseResponseCarrier> carriers{{1, 1.}, {4, 1.}};
    const std::vector<PhaseResponsePoint> points{{1, 2., 2e-10, {1., I}}};
    auto bad_points = [&](std::vector<PhaseResponsePoint> p) {
        rejects<std::invalid_argument>([&] {
            phase_response_group(channels, carriers, 2., p);
        });
    };
    bad_points({});
    bad_points({{0, 0., 0., {0., 0.}}});
    bad_points({{-1, -2., 0., {0., 0.}}});
    bad_points({points[0], points[0]});
    bad_points({{1, 2.1, 0., {0., 0.}}});
    bad_points({{1, 2., 0., {0.}}});
    bad_points({{1, 2., 0., {0., 0., 0.}}});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double value : {-1., nan, inf}) {
        bad_points({{1, 2., value, {1., I}}});
    }
    for (Complex value : {Complex{nan, 0.}, {0., nan}, {inf, 0.}, {0., inf}}) {
        bad_points({{1, 2., 0., {value, 0.}}});
    }
    for (double spacing : {0., -1., nan, inf}) {
        rejects<std::invalid_argument>([&] {
            phase_response_group(channels, carriers, spacing, points);
        });
    }
    for (const auto &bad : std::vector<std::vector<PhaseResponseCarrier>>{
             {}, {{1, 0.}}, {{1, {nan, 0.}}}, {{6, 1.}}, {{1, 1.}, {1, 1.}}}) {
        rejects<std::invalid_argument>([&] {
            phase_response_group(
                channels, bad, 2., {{1, 2., 0., std::vector<Complex>(bad.size())}});
        });
    }
    for (auto grid :
         {std::vector<ConversionChannel>{{0, 9}, {0, 10}},
          std::vector<ConversionChannel>{{0, 9}, {0, 10}, {0, 9}},
          std::vector<ConversionChannel>{{0, -1}, {0, 10}, {0, 11}},
          std::vector<ConversionChannel>{{0, 0}, {0, 1}, {0, 2}},
          std::vector<ConversionChannel>{{0, 1}, {0, std::numeric_limits<int>::max()}}}) {
        rejects<std::invalid_argument>([&] {
            phase_response_group(grid, {{1, 1.}}, 2., {{1, 2., 0., {0.}}});
        });
    }
    rejects<std::invalid_argument>([&] {
        phase_response_group(channels,
                             carriers,
                             std::numeric_limits<double>::max(),
                             {{2, std::numeric_limits<double>::infinity(), 0., {0., 0.}}});
    });
    rejects<std::invalid_argument>([&] {
        phase_response_group(std::vector<ConversionChannel>(513), carriers, 2., points);
    });
    rejects<std::invalid_argument>([&] {
        phase_response_group(channels, std::vector<PhaseResponseCarrier>(7, {1, 1.}), 2., points);
    });
    bad_points(std::vector<PhaseResponsePoint>(256, points[0]));
    const auto zero = phase_response_group(channels, carriers, 2., {{1, 2., 0., {1., I}}});
    require(zero.covariance.values.size() == 36 && zero.complementary.values.size() == 36,
            "zero PSD changed matrix shape");
    compare(zero, {conversion_detail::zero(6), conversion_detail::zero(6)});
    compare(phase_response_group(channels, carriers, 2., {{1, 2., 2e-10, {0., 0.}}}), zero);
}

void dynamic_range() {
    auto generate = [](Complex a, Complex h, double psd) {
        return phase_response_group(channels, {{1, a}}, 2., {{1, 2., psd, {h}}});
    };
    rejects<std::overflow_error>([&] {
        generate({1e300, 1e-300}, 1e-300, 2.);
    });
    rejects<std::overflow_error>([&] {
        generate({1., 1e-200}, {1., 1e-200}, 2.);
    });
    const std::vector<ConversionChannel> collision{{0, 9}, {0, 10}, {0, 11}, {0, 12}, {0, 13}};
    const double collision_wave = std::sqrt(.75 * std::numeric_limits<double>::max());
    rejects<std::overflow_error>([&] {
        phase_response_group(
            collision, {{1, collision_wave}, {3, collision_wave}}, 1., {{1, 1., 2., {1., 1.}}});
    });
    const auto ordinary = generate(1., 1., 2e-10);
    compare(generate(1e300, 1e-300, 2e-10), ordinary);
    compare(generate(1e-300, 1e300, 2e-10), ordinary);
    // Exact binary scaling keeps the oracle independent of production range logic.
    const double tiny = std::numeric_limits<double>::denorm_min();
    const auto restored = generate(std::ldexp(1., 537), 1., tiny);
    close(restored.covariance(0, 0), .5, 1.);
    close(restored.complementary(0, 2), -.5, 1.);
    const auto restored_h = generate(1., std::ldexp(1., 537), tiny);
    compare(restored, restored_h, 1.);
    for (const auto &input : std::vector<std::array<double, 3>>{
             {{1., 1., tiny}}, {{1e-300, 1., 2e-10}}, {{1e300, 1., 2.}}, {{1., 1e300, 2.}}}) {
        rejects<std::overflow_error>([&] {
            generate(input[0], input[1], input[2]);
        });
    }
    rejects<std::invalid_argument>([&] {
        generate(1., {0., std::numeric_limits<double>::quiet_NaN()}, 0.);
    });
}

void physical_network_equivalence() {
    auto A = conversion_detail::zero(6), B = A;
    for (std::size_t i = 0; i < 6; ++i) {
        A(i, i) = {.4, .1};
        B(i, i) = {-.04, .06};
        A(i, (i + 3) % 6) = {.05, -.02};
    }
    const FrequencyConversionModel common(2., channels, A, B);
    const std::vector<Complex> refs{
        {31., 12.}, {42., -3.}, {75., 8.}, {60., -15.}, {35., 4.}, {80., 20.}};
    const auto local = renormalize_power_waves(common, refs).model;
    std::vector<Complex> source(6), gamma(6, {.13, -.04});
    source[1] = {1., .3};
    source[4] = {.7, -.2};
    const auto boundary = renormalize_conversion_boundary(
        common, refs, {0, 1, 2, 3, 4, 5}, source, gamma, common.zero_noise());
    ConversionSimulationRequest request;
    request.loaded_noise = true;
    request.devices = {
        {local, boundary.source, boundary.reflection, local.zero_noise(), local.zero_noise()}};
    const auto ready = with_phase_response_noise(
        request, {{"physical", {{{0, 0, 10}}, {{0, 1, 10}}}, {{1, 2., 2e-10, {1., I}}}}});
    const auto result = analyze_conversion_simulation(ready);
    const auto coherent = common.solve_waves(source, gamma);
    for (std::size_t i = 0; i < 6; ++i) {
        const auto voltage = std::sqrt(50.) * (coherent.incident[i] + coherent.outgoing[i]);
        const auto current = (coherent.incident[i] - coherent.outgoing[i]) / std::sqrt(50.);
        close(result.channels[i].voltage_v, voltage, 1.);
        close(result.channels[i].current_a, current, 1.);
        close(result.channels[i].net_into_device_w,
              std::norm(coherent.incident[i]) - std::norm(coherent.outgoing[i]),
              1.);
    }
    Samples incident, outgoing;
    // Derive new waves from physical voltage/current, not from the production
    // boundary C/P transform. The physical model and generation reference stay fixed.
    for (const auto &z : samples({{{1., .3}, {.7, -.2}}}, {{1., I}}, false)) {
        const auto waves = common.solve_waves(z, gamma);
        std::vector<Complex> a(6), b(6);
        for (std::size_t i = 0; i < 6; ++i) {
            const auto voltage = std::sqrt(50.) * (waves.incident[i] + waves.outgoing[i]);
            const auto current = (waves.incident[i] - waves.outgoing[i]) / std::sqrt(50.);
            a[i] = (voltage + refs[i] * current) / (2. * std::sqrt(refs[i].real()));
            b[i] = (voltage - std::conj(refs[i]) * current) / (2. * std::sqrt(refs[i].real()));
        }
        incident.push_back(a);
        outgoing.push_back(b);
    }
    compare(result.waves.incident_noise, moments(incident));
    compare(result.waves.outgoing_noise, moments(outgoing));
    for (std::size_t i = 0; i < 6; ++i) {
        for (std::size_t j = 0; j < 6; ++j) {
            Complex c{}, p{};
            for (std::size_t k = 0; k < incident.size(); ++k) {
                c += incident[k][i] * std::conj(outgoing[k][j]) / double(incident.size());
                p += incident[k][i] * outgoing[k][j] / double(incident.size());
            }
            close(result.waves.incident_outgoing_noise.covariance(i, j), c);
            close(result.waves.incident_outgoing_noise.complementary(i, j), p);
        }
    }
}

void boundary_helper() {
    const auto zero = conversion_detail::zero(6);
    const std::vector<Complex> refs{
        {31., 12.}, {42., -3.}, {75., 8.}, {60., -15.}, {35., 4.}, {80., 20.}};
    FrequencyConversionModel common(2., channels, zero, zero);
    FrequencyConversionModel local_model(2., channels, zero, zero, refs);
    const std::vector<std::size_t> external{0, 1, 2, 3, 4, 5};
    std::vector<Complex> source(6), gamma(6, {.1, .03});
    source[1] = {1., .3};
    source[4] = {.7, -.2};
    const auto boundary =
        renormalize_conversion_boundary(common, refs, external, source, gamma, common.zero_noise());
    ConversionSimulationRequest request;
    request.devices.push_back({local_model,
                               boundary.source,
                               boundary.reflection,
                               local_model.zero_noise(),
                               local_model.zero_noise()});
    const std::vector<ConversionPhaseResponseGroup> groups{
        {"shared", {{{0, 0, 10}}, {{0, 1, 10}}}, {{1, 2., 2e-10, {1., I}}}}};
    const auto ready = with_phase_response_noise(request, groups);
    const std::vector<ConversionPhaseResponseGroup> independent_groups{
        {"first", {{{0, 0, 10}}}, {{1, 2., 2e-10, {1.}}}},
        {"second", {{{0, 1, 10}}}, {{1, 2., 2e-10, {I}}}}};
    const auto independent = with_phase_response_noise(request, independent_groups);
    const auto independent_common = moments(samples({{{1., .3}, {.7, -.2}}}, {{1., I}}, true));
    const auto independent_expected =
        renormalize_conversion_boundary(common, refs, external, source, gamma, independent_common)
            .source_noise;
    compare(*independent.additional_source_noise, independent_expected);
    close(independent.additional_source_noise->covariance(2, 5), 0.);
    close(independent.additional_source_noise->complementary(2, 3), 0.);
    auto invalid_measurement = ready;
    invalid_measurement.measurements.push_back(
        {"bad direction", 0, 0, 20., 4., {10}, static_cast<ConversionWaveDirection>(-1)});
    rejects<std::invalid_argument>([&] {
        analyze_conversion_simulation(invalid_measurement);
    });
    auto zero_source = request;
    zero_source.devices[0].source[1] = 0.;
    auto zero_groups = groups;
    zero_groups[0].points[0].reference_phase_rad2_per_hz = 0.;
    zero_groups[0].points[0].member_response = {0., 0.};
    rejects<std::invalid_argument>([&] {
        with_phase_response_noise(zero_source, zero_groups);
    });

    require(ready.additional_source_noise.has_value(), "helper omitted generated noise");
    require(!request.additional_source_noise, "helper mutated input request");
    const auto expected_common = moments(samples({{{1., .3}, {.7, -.2}}}, {{1., I}}, false));
    const auto expected =
        renormalize_conversion_boundary(common, refs, external, source, gamma, expected_common)
            .source_noise;
    compare(*ready.additional_source_noise, expected);
    const auto back = renormalize_conversion_boundary(local_model,
                                                      std::vector<Complex>(6, 50.),
                                                      external,
                                                      boundary.source,
                                                      boundary.reflection,
                                                      *ready.additional_source_noise);
    compare(back.source_noise, expected_common);
    compare(*with_phase_response_noise(ready, groups).additional_source_noise,
            sum(expected, expected));
    auto invalid_request = request;
    invalid_request.additional_source_noise =
        ConversionNoise{conversion_detail::zero(5), conversion_detail::zero(5)};
    rejects<std::invalid_argument>([&] {
        with_phase_response_noise(invalid_request, groups);
    });
    for (bool complementary : {false, true}) {
        auto nonfinite = local_model.zero_noise();
        (complementary ? nonfinite.complementary : nonfinite.covariance)(0, 0) =
            std::numeric_limits<double>::quiet_NaN();
        invalid_request.additional_source_noise = nonfinite;
        rejects<std::invalid_argument>([&] {
            with_phase_response_noise(invalid_request, groups);
        });
    }
    auto invalid_noise = local_model.zero_noise();
    // This negative existing process would be hidden by a large positive generated PSD.
    invalid_noise.covariance(0, 0) = -1e-11;
    invalid_request.additional_source_noise = invalid_noise;
    rejects<std::invalid_argument>([&] {
        with_phase_response_noise(invalid_request, groups);
    });
    invalid_noise = local_model.zero_noise();
    invalid_noise.covariance(0, 0) = 1e-10;
    invalid_noise.complementary(0, 0) = 2e-10;
    invalid_request.additional_source_noise = invalid_noise;
    rejects<std::invalid_argument>([&] {
        with_phase_response_noise(invalid_request, groups);
    });
    auto extra = expected;
    request.additional_source_noise = extra;
    compare(*with_phase_response_noise(request, groups).additional_source_noise,
            sum(expected, extra));
    // Value snapshots survive later group mutations and independent repeat calls.
    auto copied = groups;
    auto moved = std::move(copied);
    moved[0].points[0].member_response[0] = 0.;
    compare(*ready.additional_source_noise, expected);
    compare(*with_phase_response_noise(request, groups).additional_source_noise,
            sum(expected, extra));
    auto reject_groups = [&](std::vector<ConversionPhaseResponseGroup> bad) {
        rejects<std::invalid_argument>([&] {
            with_phase_response_noise(request, bad);
        });
    };
    reject_groups({});
    reject_groups(std::vector<ConversionPhaseResponseGroup>(513, groups[0]));
    auto nul_name = groups;
    nul_name[0].name = std::string("bad\0name", 8);
    reject_groups(nul_name);

    auto bad = groups;
    bad[0].name.clear();
    reject_groups(bad);
    bad = groups;
    bad.push_back(groups[0]);
    reject_groups(bad);
    bad = groups;
    bad[0].members[0].carrier.device = 9;
    reject_groups(bad);
    bad = groups;
    bad[0].members.push_back(bad[0].members[0]);
    reject_groups(bad);
    for (double ref : {0., -1., std::numeric_limits<double>::infinity()}) {
        rejects<std::invalid_argument>([&] {
            with_phase_response_noise(request, groups, ref);
        });
    }
    // A connected port is forbidden even when requested PSD and response are zero.
    ConversionSimulationRequest connected;
    const std::vector<ConversionChannel> one_port{{0, 9}, {0, 10}, {0, 11}};
    const auto z3 = conversion_detail::zero(3);
    FrequencyConversionModel m(2., one_port, z3, z3);
    connected.devices = {
        {m, std::vector<Complex>(3), std::vector<Complex>(3), m.zero_noise(), m.zero_noise()},
        {m, std::vector<Complex>(3), std::vector<Complex>(3), m.zero_noise(), m.zero_noise()}};
    connected.connections = {{0, 0, 1, 0}};
    rejects<std::invalid_argument>([&] {
        with_phase_response_noise(connected, {{"internal", {{{0, 0, 10}}}, {{1, 2., 0., {0.}}}}});
    });
}
} // namespace

int main() {
    try {
        statistics_and_transport();
        phase_scan_collision_and_offsets();
        real_equivalence_and_ownership();
        validation();
        dynamic_range();
        boundary_helper();
        physical_network_equivalence();
        std::cout << "phase response independent statistics, mapping, ownership and boundary tests "
                     "passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
