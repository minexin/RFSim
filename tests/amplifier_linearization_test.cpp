#include "rfmodel/amplifier_linearization.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>

using namespace rfmodel;

void gain_derivatives() {
    const SaturatingFundamentalCompression model(20., 20., 23.);
    const P1dBFundamentalCompression cubic(20., 20.);
    const double p1 = model.input_p1db_watts(), anchor = std::sqrt(p1);
    near(model.gain_response(0.).amplitude_gain, 10.);
    near(model.gain_response(0.).radial_amplitude_slope, 10.);
    near(model.gain_response(0.).radial_gain_difference, 0.);
    for (double ratio : {.001, .1, .9, 1., 1.001, 2., 5., 100.}) {
        const double drive = std::sqrt(p1 * ratio), step = anchor * 1e-6;
        const auto response = model.gain_response(p1 * ratio);
        const double difference = (model.transmit_fundamental(drive + step).real() -
                                   model.transmit_fundamental(drive - step).real()) /
                                  (2. * step);
        require(std::abs(difference - response.radial_amplitude_slope) < 2e-5,
                "radial derivative differs from old response finite difference");
        near(response.amplitude_gain, model.amplitude_gain(p1 * ratio));
        if (ratio <= 1.) {
            near(response.radial_amplitude_slope,
                 cubic.gain_response(p1 * ratio).radial_amplitude_slope);
        }
    }
    const auto tiny = model.gain_response(p1 * 1e-20);
    require(tiny.radial_gain_difference < 0., "small-drive coupling was cancelled numerically");
    const double headroom = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1);
    const double slope = 10. * (3. * std::pow(10., -.05) - 2.);
    const double drive = anchor + 20. * headroom / slope;
    const auto far = model.gain_response(drive * drive);
    require(far.radial_amplitude_slope > 0., "representable saturated slope was lost");
    near(far.radial_amplitude_slope / (4. * slope * std::exp(-40.)), 1.);
    require(std::isfinite(model.gain_response(1e300).amplitude_gain), "extreme drive overflowed");
    rejects<std::invalid_argument>([&] {
        model.gain_response(-1.);
    });
    rejects<std::out_of_range>([&] {
        cubic.gain_response(2. * p1);
    });
}

void coupled_derivatives() {
    const SaturatingFundamentalCompression amplifier(20., 20., 23.);
    const double p1 = amplifier.input_p1db_watts();
    const std::vector<ConversionChannel> channels{{0, 11}, {0, 13}, {1, 11}, {1, 13}};
    for (const auto &ports : {std::vector<std::size_t>{0}, std::vector<std::size_t>{0, 1}}) {
        for (double ratio : {0., .3, 1., 4.}) {
            std::vector<Complex> wave{.8, Complex{0., .6}, Complex{.2, .1}, Complex{.1, -.2}};
            double norm = 0.;
            for (std::size_t i = 0; i < wave.size(); ++i) {
                if (std::find(ports.begin(), ports.end(), channels[i].port) != ports.end()) {
                    norm += std::norm(wave[i]);
                }
            }
            for (auto &value : wave) {
                value *= std::sqrt(ratio * p1 / norm);
            }
            const auto result =
                linearize_saturating_amplifier(1., channels, wave, amplifier, 0, 1, ports);
            for (std::size_t i = 0; i < 2; ++i) {
                near(result.outgoing[i], 0.);
                near(result.outgoing[i + 2], amplifier.transmit_fundamental(wave[i], p1 * ratio));
            }
            const double step = std::sqrt(p1) * 1e-6;
            for (std::size_t j = 0; j < 4; ++j) {
                for (auto direction : {Complex{1., 0.}, Complex{0., 1.}}) {
                    auto high = wave, low = wave;
                    high[j] += step * direction;
                    low[j] -= step * direction;
                    const auto plus =
                        linearize_saturating_amplifier(1., channels, high, amplifier, 0, 1, ports);
                    const auto minus =
                        linearize_saturating_amplifier(1., channels, low, amplifier, 0, 1, ports);
                    for (std::size_t i = 0; i < 4; ++i) {
                        const auto expected =
                            result.jacobian.direct()(i, j) * direction +
                            result.jacobian.conjugate()(i, j) * std::conj(direction);
                        require(std::abs((plus.outgoing[i] - minus.outgoing[i]) / (2. * step) -
                                         expected) < 2e-5,
                                "amplifier cross-channel derivative differs");
                    }
                }
            }
            const auto response = amplifier.gain_response(ratio * p1);
            for (std::size_t i = 0; i < 2; ++i) {
                Complex radial{}, phase{};
                for (std::size_t j = 0; j < 4; ++j) {
                    radial += result.jacobian.direct()(i + 2, j) * wave[j] +
                              result.jacobian.conjugate()(i + 2, j) * std::conj(wave[j]);
                    phase += result.jacobian.direct()(i + 2, j) * Complex{0., 1.} * wave[j] +
                             result.jacobian.conjugate()(i + 2, j) * Complex{0., -1.} *
                                 std::conj(wave[j]);
                }
                near(radial, response.radial_amplitude_slope * wave[i]);
                near(phase, Complex{0., 1.} * result.outgoing[i + 2]);
            }
            if (ratio > 0.) {
                require(std::abs(result.jacobian.direct()(2, 1)) > 0.,
                        "cross-tone compression missing");
            }
        }
    }
    const auto blocked = linearize_saturating_amplifier(
        1., {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {0., .01, 0., 0.}, amplifier);
    near(blocked.outgoing[2], 0.);
    for (const auto &ports : {std::vector<std::size_t>{1},
                              std::vector<std::size_t>{0, 0},
                              std::vector<std::size_t>{0, 2}}) {
        rejects<std::invalid_argument>([&] {
            linearize_saturating_amplifier(1., channels, {0., 0., 0., 0.}, amplifier, 0, 1, ports);
        });
    }
    rejects<std::invalid_argument>([&] {
        linearize_saturating_amplifier(
            1., {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {1., 0., 0., 0.}, amplifier);
    });
    rejects<std::invalid_argument>([&] {
        linearize_saturating_amplifier(1., {{0, 1}, {1, 2}}, {0., 0.}, amplifier);
    });
}

void cross_frequency_noise() {
    const SaturatingFundamentalCompression amplifier(20., 20., 23.);
    const double power = amplifier.input_p1db_watts() * .8;
    const double radius = std::sqrt(power);
    const std::vector<Complex> wave{.8 * radius, .6 * radius, 0., 0.};
    const auto point =
        linearize_saturating_amplifier(1., {{0, 11}, {0, 13}, {1, 11}, {1, 13}}, wave, amplifier);
    auto noise = point.jacobian.zero_noise();
    noise.covariance(0, 0) = noise.covariance(1, 1) = 1e-9;
    const auto result = point.jacobian.analyze(
        std::vector<Complex>(4), std::vector<Complex>(4), noise, point.jacobian.zero_noise());
    const auto gain = amplifier.gain_response(power);
    const double coupled = .5 * (gain.radial_amplitude_slope * gain.radial_amplitude_slope -
                                 gain.amplitude_gain * gain.amplitude_gain);
    near(result.outgoing_noise.covariance(2, 3) / 1e-9, coupled * .8 * .6);
    near(result.outgoing_noise.complementary(2, 3) / 1e-9, coupled * .8 * .6);
    near(result.outgoing_noise.covariance(2, 2) / 1e-9,
         gain.amplitude_gain * gain.amplitude_gain + coupled * .8 * .8);
}

void nonlinear_feedback() {
    const SaturatingFundamentalCompression amplifier(20., 20., 23.);
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}};
    const auto zero = conversion_detail::zero(2);
    FrequencyConversionModel base(1., channels, zero, zero);
    ConversionDevice active{base, {0., 0.}, {0., 0.}, base.zero_noise(), base.zero_noise()};
    auto feedback = active;
    auto scattering = zero;
    constexpr double beta = -.2, source = .2, density = 1e-9;
    scattering(1, 0) = beta;
    feedback.model = FrequencyConversionModel(1., channels, scattering, zero);
    feedback.intrinsic_noise.covariance(1, 1) = density;
    ConversionNonlinearDevice law{0, [=](const std::vector<Complex> &wave) {
                                      return linearize_saturating_amplifier(
                                          1., channels, wave, amplifier);
                                  }};
    const auto result = solve_conversion_operating_point({active, feedback},
                                                         {{0, 1, 1, 0}, {0, 0, 1, 1}},
                                                         {law},
                                                         {},
                                                         {},
                                                         true,
                                                         nullptr,
                                                         {0., 0., 0., source});
    // Independent scalar bisection of the original cubic/tanh response.
    auto amplitude = [&](double input) {
        const double p1 = std::pow(10., (20. - 20. + 1. - 30.) / 10.);
        if (input * input <= p1) {
            return 10. * input * (1. - (1. - std::pow(10., -.05)) * input * input / p1);
        }
        const double head = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1);
        const double slope = 10. * (3. * std::pow(10., -.05) - 2.);
        return std::sqrt(.1) + head * std::tanh(slope * (input - std::sqrt(p1)) / head);
    };
    double lower = 0., upper = source;
    for (int i = 0; i < 100; ++i) {
        const double middle = .5 * (lower + upper);
        if (middle - beta * amplitude(middle) < source) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    const double input = .5 * (lower + upper);
    require(std::abs(result.waves.incident[0] - input) < 1e-10, "compressed feedback root differs");
    require(std::abs(result.waves.outgoing[1] - amplitude(input)) < 1e-10,
            "compressed feedback output differs");
    require(result.iterations > 1 && result.scaled_residual <= 1.,
            "feedback convergence diagnostics differ");
    const auto gain = amplifier.gain_response(std::norm(result.waves.incident[0]));
    const double radial = gain.radial_amplitude_slope / (1. - beta * gain.radial_amplitude_slope);
    const double tangent = gain.amplitude_gain / (1. - beta * gain.amplitude_gain);
    near(result.waves.outgoing_noise.covariance(1, 1) / density,
         .5 * (radial * radial + tangent * tangent));
    near(result.waves.outgoing_noise.complementary(1, 1) / density,
         .5 * (radial * radial - tangent * tangent));
}

int main() {
    try {
        gain_derivatives();
        coupled_derivatives();
        cross_frequency_noise();
        nonlinear_feedback();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
