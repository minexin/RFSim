#include "rfmodel/conversion/conversion_operating_point.hpp"
#include "rfmodel/conversion/am_pm_linearization.hpp"
#include "rfmodel/conversion/amplifier_linearization.hpp"
#include "rfmodel/conversion/highorder_linearization.hpp"
#include "rfmodel/conversion/mixer_linearization.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

ConversionDevice empty_device(const std::vector<ConversionChannel> &channels) {
    const auto n = channels.size();
    FrequencyConversionModel model(
        1., channels, conversion_detail::zero(n), conversion_detail::zero(n));
    return {model,
            std::vector<Complex>(n),
            std::vector<Complex>(n),
            model.zero_noise(),
            model.zero_noise()};
}

void fixed_mixer_derivatives() {
    const std::vector<ConversionChannel> channels{{0, 0},
                                                  {0, 4},
                                                  {1, 0},
                                                  {1, 3},
                                                  {1, 5},
                                                  {2, 0},
                                                  {2, 1},
                                                  {2, 3},
                                                  {2, 4},
                                                  {2, 5},
                                                  {2, 7},
                                                  {2, 9}};
    const std::vector<Complex> waves{
        .2, {1., .3}, .1, {2., -.4}, {.5, .7}, 0., 0., 0., 0., 0., 0., 0.};
    const auto model = linearize_bilinear_mixer(1., channels, waves, 0., 2.);
    // Independent signed Fourier convolution, including DC and two LO tones.
    std::map<int, Complex> rf, lo, spectrum;
    for (std::size_t i = 0; i < 5; ++i) {
        auto &table = channels[i].port == 0 ? rf : lo;
        const int bin = channels[i].bin;
        table[bin] = bin ? waves[i] / std::sqrt(2.) : waves[i];
        if (bin) {
            table[-bin] = std::conj(table[bin]);
        }
    }
    for (const auto &r : rf) {
        for (const auto &l : lo) {
            spectrum[r.first + l.first] += std::sqrt(2.) * .5 * r.second * l.second;
        }
    }
    for (std::size_t i = 5; i < channels.size(); ++i) {
        const int bin = channels[i].bin;
        near(model.operating_outgoing[i], spectrum[bin] * (bin ? std::sqrt(2.) : 1.));
    }
    constexpr double step = 1e-6;
    for (std::size_t j = 0; j < 5; ++j) {
        for (auto direction : {Complex{1., 0.}, Complex{0., 1.}}) {
            if (!channels[j].bin && direction.imag() != 0.) {
                continue;
            }
            auto plus = waves, minus = waves;
            plus[j] += step * direction;
            minus[j] -= step * direction;
            const auto high =
                linearize_bilinear_mixer(1., channels, plus, 0., 2.).operating_outgoing;
            const auto low =
                linearize_bilinear_mixer(1., channels, minus, 0., 2.).operating_outgoing;
            for (std::size_t i = 0; i < channels.size(); ++i) {
                const auto derivative =
                    model.incremental_model.direct()(i, j) * direction +
                    model.incremental_model.conjugate()(i, j) * std::conj(direction);
                require(std::abs((high[i] - low[i]) / (2. * step) - derivative) < 1e-9,
                        "fixed-coefficient mixer derivative differs from Fourier product");
            }
        }
    }
    auto doubled = waves;
    for (std::size_t i = 2; i < 5; ++i) {
        doubled[i] *= 2.;
    }
    const auto driven = linearize_bilinear_mixer(1., channels, doubled, 0., 2.);
    for (std::size_t i = 0; i < channels.size(); ++i) {
        near(driven.operating_outgoing[i], 2. * model.operating_outgoing[i]);
    }
    linearize_bilinear_mixer(1., channels, std::vector<Complex>(channels.size()), 0., 2.);
    rejects<std::invalid_argument>([&] {
        linearize_bilinear_mixer(1., {{0, 4}, {1, 3}, {2, 1}}, {0., 0., 0.}, 0., 1.);
    });
    rejects<std::invalid_argument>([&] {
        linearize_bilinear_mixer(1., channels, waves, 0., 0.);
    });
}

void damped_cubic() {
    auto device = empty_device({{0, 1}});
    device.source[0] = 1.;
    device.reflection[0] = 1.;
    device.source_noise.covariance(0, 0) = 1e-9;
    ConversionNonlinearDevice cubic{
        0, [](const std::vector<Complex> &a) {
            return ConversionLinearization{
                FrequencyConversionModel(
                    1., {{0, 1}}, {1, {-2. * std::norm(a[0])}}, {1, {-a[0] * a[0]}}),
                {-std::norm(a[0]) * a[0]}};
        }};
    const auto result = solve_conversion_operating_point({device}, {}, {cubic}, {.01}, {}, true);
    const double a = result.waves.incident[0].real();
    require(std::abs(a + a * a * a - 1.) < 1e-9, "cubic true residual differs");
    near(result.waves.outgoing[0], -a * a * a);
    require(result.iterations > 1 && result.backtracks > 0 && result.scaled_residual <= 1.,
            "damped Newton diagnostics differ");
    // Radial and tangential noise gains differ for -abs(a)^2*a.
    const double radial = -3. * a * a / (1. + 3. * a * a);
    const double tangent = -a * a / (1. + a * a);
    near(result.waves.outgoing_noise.covariance(0, 0) / 1e-9,
         .5 * (radial * radial + tangent * tangent));
    near(result.waves.outgoing_noise.complementary(0, 0) / 1e-9,
         .5 * (radial * radial - tangent * tangent));
    const double radial_incident = 1. / (1. + 3. * a * a);
    const double tangent_incident = 1. / (1. + a * a);
    near(result.waves.incident_noise.covariance(0, 0) / 1e-9,
         .5 * (radial_incident * radial_incident + tangent_incident * tangent_incident));
    const ConversionNoise extra{{1, {.5e-9}}, {1, {0.}}};
    const auto correlated =
        solve_conversion_operating_point({device}, {}, {cubic}, {.01}, {}, true, &extra);
    near(correlated.waves.outgoing_noise.covariance(0, 0) / 1.5e-9,
         result.waves.outgoing_noise.covariance(0, 0) / 1e-9);
    auto invalid_noise = device;
    invalid_noise.source_noise.covariance(0, 0) = -1e-9;
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point({invalid_noise}, {}, {cubic});
    });
    const auto warm =
        solve_conversion_operating_point({device}, {}, {cubic}, result.waves.incident);
    require(warm.iterations == 0, "converged initial point was not retained");
    ConversionOperatingOptions short_run;
    short_run.max_iterations = 1;
    rejects<std::runtime_error>([&] {
        solve_conversion_operating_point({device}, {}, {cubic}, {.01}, short_run);
    });
    short_run.max_iterations = 50;
    short_run.max_backtracks = 0;
    rejects<std::runtime_error>([&] {
        solve_conversion_operating_point({device}, {}, {cubic}, {.01}, short_run);
    });
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point({device}, {}, {cubic, cubic});
    });
    auto bad = cubic;
    bad.evaluate = [](const std::vector<Complex> &) {
        return ConversionLinearization{FrequencyConversionModel(1., {{1, 1}}, {1, {0.}}, {1, {0.}}),
                                       {0.}};
    };
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point({device}, {}, {bad});
    });
    short_run.relative_tolerance = 0.;
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point({device}, {}, {cubic}, {}, short_run);
    });
    // Unity positive feedback with a nonzero source has no finite solution.
    ConversionNonlinearDevice singular{
        0, [](const std::vector<Complex> &a) {
            return ConversionLinearization{
                FrequencyConversionModel(1., {{0, 1}}, {1, {1.}}, {1, {0.}}), {a[0]}};
        }};
    rejects<std::domain_error>([&] {
        solve_conversion_operating_point({device}, {}, {singular});
    });
}

void wired_bilinear_feedback() {
    const std::vector<ConversionChannel> channels{{0, 0}, {1, 0}, {2, 0}};
    auto mixer = empty_device(channels), splitter = empty_device(channels);
    auto a = conversion_detail::zero(3);
    a(1, 0) = .1;
    a(2, 0) = .2;
    splitter.model = FrequencyConversionModel(1., channels, a, conversion_detail::zero(3));
    const std::vector<ConversionPortConnection> wires{{0, 2, 1, 0}, {0, 0, 1, 1}, {0, 1, 1, 2}};
    ConversionNonlinearDevice law{
        0, [channels](const std::vector<Complex> &wave) {
            const auto m = linearize_bilinear_mixer(1., channels, wave, -10. * std::log10(2.), 1.);
            return ConversionLinearization{m.incremental_model, m.operating_outgoing};
        }};
    const std::vector<Complex> offset{0., 0., 0., 0., 1., 2.};
    const auto result = solve_conversion_operating_point(
        {mixer, splitter}, wires, {law}, {}, {}, false, nullptr, offset);
    // b = (1 + .1*b)*(2 + .2*b); choose the branch reached from zero.
    const double b = (30. - std::sqrt(500.)) / 2.;
    require(std::abs(result.waves.outgoing[2] - b) < 1e-8, "wired bilinear root differs");
    require(std::abs(result.waves.incident[0] - (1. + .1 * b)) < 1e-8, "RF feedback differs");
    require(std::abs(result.waves.incident[1] - (2. + .2 * b)) < 1e-8, "LO feedback differs");
    for (std::size_t i = 0; i < 6; ++i) {
        require(result.waves.incident[i].imag() == 0. && result.waves.outgoing[i].imag() == 0.,
                "DC became complex");
    }
    require(result.scaled_residual <= 1., "nonlinear wire residual failed");
    auto driven = mixer;
    driven.source[0] = 1.;
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point(
            {driven, splitter}, wires, {law}, {}, {}, false, nullptr, offset);
    });
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point(
            {mixer, splitter}, wires, {law}, {Complex{0., 1.}, 0., 0., 0., 0., 0.});
    });
}

// Independent scalar reference for the 20 dB / 20 dBm / 23 dBm amplifier.
// Do not use the native gain/limiter response to construct expected derivatives.
struct ScalarCompression {
    double amplitude, derivative;
};

ScalarCompression scalar_compression(double radius) {
    const double q = std::pow(10., -.05), p1 = std::sqrt(std::pow(10., -2.9));
    if (radius <= p1) {
        const double reduction = (1. - q) * (radius / p1) * (radius / p1);
        return {10. * radius * (1. - reduction), 10. * (1. - 3. * reduction)};
    }
    const double headroom = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1);
    const double slope = 10. * (3. * q - 2.);
    const double argument = slope * (radius - p1) / headroom;
    return {std::sqrt(.1) + headroom * std::tanh(argument),
            slope / std::pow(std::cosh(argument), 2)};
}

// Integrate the specified degrees/dB slope, independently of the production
// amplitude-domain antiderivative. The omitted tail below -160 dB is negligible.
double integrated_output_phase(double amplitude) {
    const double end = std::min(20. * std::log10(amplitude / .1), 20. * std::log10(1.5));
    constexpr int intervals = 12000;
    const double step = (end + 160.) / intervals;
    double sum = 0.;
    for (int i = 0; i <= intervals; ++i) {
        const double u = std::pow(10., (-160. + i * step) / 20.);
        const double weight = i == 0 || i == intervals ? 1. : (i % 2 ? 4. : 2.);
        sum += weight * -5. * u * u * (3. - 2. * u);
    }
    return sum * step / 3. * std::acos(-1.) / 180.;
}

void snapshot_close(Complex actual, Complex expected, double scale, const char *message) {
    // Relative to a physical wave/derivative product, never a 1 W/Hz floor.
    // This also keeps the small third-harmonic C/P entries meaningful.
    if (!(std::abs(actual - expected) <= 5e-10 * scale)) {
        std::cerr << message << ": " << actual << " != " << expected << " (scale " << scale
                  << ")\n";
        throw std::runtime_error(message);
    }
}

void check_quadrature_noise(const ConversionNoise &actual,
                            const std::vector<Complex> &left_radial,
                            const std::vector<Complex> &left_tangent,
                            const std::vector<Complex> &right_radial,
                            const std::vector<Complex> &right_tangent,
                            double variance) {
    const auto n = left_radial.size();
    require(actual.covariance.ports == n && actual.complementary.ports == n,
            "snapshot noise dimensions differ");
    // A proper incident thermal wave has two independent real quadratures,
    // each of variance kT/2. Outer products give every entry, including zeros,
    // cross-frequency terms, and the non-Hermitian incident/outgoing blocks.
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const auto c = .5 * (left_radial[i] * std::conj(right_radial[j]) +
                                 left_tangent[i] * std::conj(right_tangent[j]));
            const auto p =
                .5 * (left_radial[i] * right_radial[j] + left_tangent[i] * right_tangent[j]);
            const double scale = .5 * (std::abs(left_radial[i]) * std::abs(right_radial[j]) +
                                       std::abs(left_tangent[i]) * std::abs(right_tangent[j]));
            snapshot_close(actual.covariance(i, j) / variance, c, scale, "snapshot C differs");
            snapshot_close(actual.complementary(i, j) / variance, p, scale, "snapshot P differs");
        }
    }
}

void check_converged_snapshot(const std::vector<ConversionChannel> &channels,
                              const ConversionNonlinearDevice &law,
                              double radius,
                              double input_phase,
                              const std::vector<Complex> &outgoing,
                              const std::vector<Complex> &radial,
                              const std::vector<Complex> &tangent) {
    constexpr double temperature = 290., kt = 1.380649e-23 * temperature;
    const auto n = channels.size();
    auto device = empty_device(channels);
    const auto direction = std::polar(1., input_phase);
    device.source[0] = radius * direction;
    device.source_noise.covariance(0, 0) = kt;
    // A stale initial Jacobian has different compression, limiter, and AM/PM
    // slopes. Starting at the source itself would not exercise final rebuilding.
    std::vector<Complex> initial(n), fixed_offset(n);
    initial[0] = std::polar(.08, input_phase + .4);
    if (input_phase != 0.) {
        for (std::size_t i = 1; i < n; ++i) {
            fixed_offset[i] = Complex{.003, -.002} * double(i);
        }
    }
    const auto solved = solve_conversion_operating_point(
        {device}, {}, {law}, initial, {}, true, nullptr, fixed_offset);
    require(solved.iterations > 0 && solved.scaled_residual <= 1.,
            "snapshot fixture did not converge from its distinct initial point");
    require(solved.linearized_devices.size() == 1 && solved.output_offset.size() == n,
            "converged snapshot dimensions differ");

    // Compose only the returned accepted-point snapshot, with no fresh call to
    // the nonlinear law. Both the replay and reference experiment must use it.
    FrequencyConversionNetwork rebuilt(1.);
    rebuilt.add(solved.linearized_devices.front());
    const auto replay = rebuilt.analyze(true, nullptr, solved.output_offset);
    const auto &jacobian = solved.linearized_devices.front().model;
    std::vector<Complex> incident_radial(n), incident_tangent(n);
    incident_radial[0] = direction;
    incident_tangent[0] = Complex{0., 1.} * direction;
    for (std::size_t i = 0; i < n; ++i) {
        const auto actual_radial =
            jacobian.direct()(i, 0) * direction + jacobian.conjugate()(i, 0) * std::conj(direction);
        const auto actual_tangent = jacobian.direct()(i, 0) * incident_tangent[0] +
                                    jacobian.conjugate()(i, 0) * std::conj(incident_tangent[0]);
        snapshot_close(actual_radial, radial[i], std::abs(radial[i]), "final radial slope differs");
        snapshot_close(
            actual_tangent, tangent[i], std::abs(tangent[i]), "final tangential slope differs");
        for (std::size_t j = 1; j < n; ++j) {
            snapshot_close(jacobian.direct()(i, j), 0., 0., "inactive direct column differs");
            snapshot_close(jacobian.conjugate()(i, j), 0., 0., "inactive conjugate column differs");
        }
        const double output_scale = std::abs(outgoing[i]) + std::abs(fixed_offset[i]);
        snapshot_close(solved.output_offset[i],
                       outgoing[i] + fixed_offset[i] - radius * radial[i],
                       output_scale + radius * std::abs(radial[i]),
                       "final affine offset differs");
        for (const auto *waves : {&solved.waves, &replay}) {
            snapshot_close(waves->incident[i],
                           device.source[i],
                           std::abs(device.source[i]),
                           "snapshot incident wave differs");
            snapshot_close(waves->outgoing[i],
                           outgoing[i] + fixed_offset[i],
                           output_scale,
                           "snapshot outgoing wave differs");
            const double input_gain = i == 0 ? 1. : 0.;
            const double output_gain = .5 * (std::norm(radial[i]) + std::norm(tangent[i]));
            snapshot_close(waves->net_noise_into_device_w_per_hz[i] / kt,
                           input_gain - output_gain,
                           input_gain + output_gain,
                           "snapshot net noise differs");
        }
    }
    for (const auto *waves : {&solved.waves, &replay}) {
        check_quadrature_noise(waves->outgoing_noise, radial, tangent, radial, tangent, kt);
        check_quadrature_noise(waves->incident_noise,
                               incident_radial,
                               incident_tangent,
                               incident_radial,
                               incident_tangent,
                               kt);
        check_quadrature_noise(
            waves->incident_outgoing_noise, incident_radial, incident_tangent, radial, tangent, kt);
    }
    for (std::size_t output = 1; output < n; ++output) {
        const auto metric = rebuilt.reference_noise_analysis({0}, {0}, output, temperature);
        const double gain = .5 * (std::norm(radial[output]) + std::norm(tangent[output]));
        snapshot_close(metric.reference_gain, gain, gain, "snapshot reference gain differs");
        snapshot_close(metric.reference_output_noise_w_per_hz / kt,
                       gain,
                       gain,
                       "snapshot reference thermal noise differs");
        snapshot_close(
            metric.output_noise_w_per_hz / kt, gain, gain, "snapshot output noise differs");
        near(metric.noise_factor, 1.);
        near(metric.noise_figure_db, 0.);
        require(std::abs(metric.equivalent_input_temperature_k) < 1e-9,
                "noiseless nonlinear snapshot acquired an excess noise temperature");
    }
}

void am_pm_snapshot_reference_noise() {
    // Native composition counterpart of test_nf_rebuild_uses_phase_derivative.
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}};
    const SaturatingFundamentalCompression amplifier(20., 20., 23.);
    const LegacyPointDerivativeAmPm phase(-5., 10.);
    const ConversionNonlinearDevice law{
        0, [&](const std::vector<Complex> &wave) {
            return apply_output_am_pm(
                linearize_saturating_amplifier(1., channels, wave, amplifier), phase, {1});
        }};
    for (const auto &fixture : {std::pair<double, double>{.01, 0.}, {.01, .47}, {.06, -.31}}) {
        const double radius = fixture.first, angle = fixture.second;
        const auto scalar = scalar_compression(radius);
        const double u = scalar.amplitude / .1;
        // d(phi)/d(log amplitude), from the specified dB-domain derivative.
        const double phase_slope =
            u >= 1.5
                ? 0.
                : (20. / std::log(10.)) * (-5. * std::acos(-1.) / 180.) * u * u * (3. - 2. * u);
        const auto rotation = std::polar(1., angle + integrated_output_phase(scalar.amplitude));
        check_converged_snapshot(channels,
                                 law,
                                 radius,
                                 angle,
                                 {0., rotation * scalar.amplitude},
                                 {0., rotation * scalar.derivative * Complex{1., phase_slope}},
                                 {0., rotation * Complex{0., scalar.amplitude / radius}});
    }
}

void highorder_snapshot_reference_noise() {
    // Native composition counterpart of test_noise_analysis_reconstructs_converged_model.
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}, {1, 3}};
    const CoherentHighOrderAmplifier amplifier(20., 20., 23., {0., .1});
    const ConversionNonlinearDevice law{0, [&](const std::vector<Complex> &wave) {
                                            return linearize_highorder_amplifier(
                                                1., channels, wave, amplifier, {1});
                                        }};
    for (const auto &fixture : {std::pair<double, double>{.03, 0.}, {.03, .47}, {.06, -.31}}) {
        const double radius = fixture.first, angle = fixture.second;
        const auto scalar = scalar_compression(radius);
        const double knee = std::sqrt(std::pow(10., -3.4));
        const double headroom = std::sqrt(std::pow(10., -2.8)) - knee;
        const double argument = (radius - knee) / headroom;
        const double limited = radius <= knee ? radius : knee + headroom * std::tanh(argument);
        const double derivative = radius <= knee ? 1. : 1. / std::pow(std::cosh(argument), 2);
        // cos^3(x)=(3*cos(x)+cos(3*x))/4 with c3=.1 and R=50.
        const double cubic = limited * limited * limited;
        const double fundamental = scalar.amplitude + 7.5 * cubic, harmonic = 2.5 * cubic;
        const double harmonic_radial = 7.5 * limited * limited * derivative;
        const auto first = std::polar(1., angle), third = std::polar(1., 3. * angle);
        check_converged_snapshot(
            channels,
            law,
            radius,
            angle,
            {0., first * fundamental, third * harmonic},
            {0., first * (scalar.derivative + 3. * harmonic_radial), third * harmonic_radial},
            {0.,
             first * Complex{0., fundamental / radius},
             third * Complex{0., 3. * harmonic / radius}});
    }
}

int main() {
    try {
        fixed_mixer_derivatives();
        damped_cubic();
        wired_bilinear_feedback();
        am_pm_snapshot_reference_noise();
        highorder_snapshot_reference_noise();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
