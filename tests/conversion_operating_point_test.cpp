#include "rfmodel/conversion_operating_point.hpp"
#include "rfmodel/mixer_linearization.hpp"
#include "test_support.hpp"
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

int main() {
    try {
        fixed_mixer_derivatives();
        damped_cubic();
        wired_bilinear_feedback();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
