#include "rfmodel/polynomial_linearization.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>

using namespace rfmodel;

// Independent time-domain sampling with direct powers and Fourier projection.
// All test products lie below Nyquist; no circular-convolution approximation.
std::vector<Complex> sampled_response(const std::vector<ConversionChannel> &channels,
                                      const std::vector<Complex> &waves,
                                      const std::vector<double> &coefficients,
                                      double reference,
                                      const std::vector<Complex> &direction = {}) {
    constexpr int samples = 512;
    const double pi = std::acos(-1.), root = std::sqrt(reference);
    std::vector<Complex> result(channels.size());
    for (int sample = 0; sample < samples; ++sample) {
        const double phase = 2. * pi * sample / samples;
        double voltage = 0., variation = 0.;
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].port != 0) {
                continue;
            }
            const auto rotating = std::polar(1., phase * channels[i].bin);
            const double scale = root * (channels[i].bin ? std::sqrt(2.) : 1.);
            voltage += scale * (waves[i] * rotating).real();
            if (!direction.empty()) {
                variation += scale * (direction[i] * rotating).real();
            }
        }
        double output = 0.;
        for (std::size_t order = 0; order < coefficients.size(); ++order) {
            if (direction.empty()) {
                output += coefficients[order] * std::pow(voltage, static_cast<int>(order));
            } else if (order) {
                output += order * coefficients[order] *
                          std::pow(voltage, static_cast<int>(order - 1)) * variation;
            }
        }
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].port == 1) {
                const double scale = (channels[i].bin ? std::sqrt(2.) : 1.) / root;
                result[i] +=
                    scale * output * std::polar(1., -phase * channels[i].bin) / double(samples);
            }
        }
    }
    return result;
}

void structural_support() {
    require(polynomial_output_bins({1}, MemorylessPolynomial({0., 0., 1.})) ==
                std::vector<int>({0, 2}),
            "square must include rectified DC and second harmonic");
    require(polynomial_output_bins({1}, MemorylessPolynomial({0., 0., 0., 1.})) ==
                std::vector<int>({1, 3}),
            "cubic support differs");
    std::vector<double> coefficients(12);
    coefficients[11] = 1.;
    require(polynomial_output_bins({1}, MemorylessPolynomial(coefficients)) ==
                std::vector<int>({1, 3, 5, 7, 9, 11}),
            "eleventh-order support differs");
    require(polynomial_output_bins({0, 2}, MemorylessPolynomial({0., 0., 0., 1.})) ==
                std::vector<int>({0, 2, 4, 6}),
            "DC bias must permit even harmonics in an odd polynomial");
    require(polynomial_output_bins({1, 2}, MemorylessPolynomial({0.})).empty(),
            "zero polynomial support differs");
    const auto derivative = MemorylessPolynomial({2., 3., 4., 5.}).derivative();
    for (std::size_t i = 0; i < 4; ++i) {
        near(derivative.coefficient(i), std::vector<double>{3., 8., 15., 0.}[i]);
    }
    near(MemorylessPolynomial({3.}).derivative().coefficient(0), 0.);
    rejects<std::invalid_argument>([] {
        polynomial_output_bins({}, MemorylessPolynomial({1.}));
    });
    rejects<std::invalid_argument>([] {
        polynomial_output_bins({1, 1}, MemorylessPolynomial({1.}));
    });
    rejects<std::invalid_argument>([] {
        polynomial_output_bins({-1}, MemorylessPolynomial({1.}));
    });
    rejects<std::overflow_error>([] {
        polynomial_output_bins({std::numeric_limits<int>::max()},
                               MemorylessPolynomial({0., 0., 1.}));
    });
    rejects<std::length_error>([] {
        std::vector<double> coefficients(12);
        coefficients[11] = 1.;
        polynomial_output_bins({1, 13, 169, 2197, 28561, 371293},
                               MemorylessPolynomial(coefficients));
    });
    rejects<std::overflow_error>([] {
        MemorylessPolynomial({0., 0., std::numeric_limits<double>::max()}).derivative();
    });
    // Even a zero working point must declare every possible future product.
    rejects<std::invalid_argument>([] {
        linearize_polynomial_amplifier(
            1., {{0, 1}, {1, 1}}, {0., 0.}, MemorylessPolynomial({0., 1., 0., .1}));
    });
    const int highest = std::numeric_limits<int>::max();
    const auto linear = linearize_polynomial_amplifier(
        1., {{0, highest}, {1, highest}}, {.01, 0.}, MemorylessPolynomial({0., 2.}));
    near(linear.outgoing[1], .02);
}

void sampled_values_and_derivatives() {
    for (auto degree : {2, 3, 5, 11}) {
        std::vector<double> coefficients(degree + 1);
        coefficients[0] = .01;
        coefficients[1] = 1.3;
        for (int order = 2; order <= degree; ++order) {
            coefficients[order] = (order % 2 ? -.02 : .02) / order;
        }
        const MemorylessPolynomial polynomial(coefficients);
        std::vector<ConversionChannel> channels{{0, 0}, {0, 2}, {0, 5}};
        for (int bin = 0; bin <= 5 * degree; ++bin) {
            channels.push_back({1, bin});
        }
        std::vector<Complex> waves(channels.size());
        waves[0] = .02;
        waves[1] = {.035, .01};
        waves[2] = {.018, -.006};
        for (auto reference : {1., 50., 75.}) {
            const auto point =
                linearize_polynomial_amplifier(1., channels, waves, polynomial, 0, 1, reference);
            const auto expected = sampled_response(channels, waves, coefficients, reference);
            const MatchedPolynomialAmplifier existing("reference", coefficients, reference);
            const auto old = existing.transmit({1., {{0, waves[0]}, {2, waves[1]}, {5, waves[2]}}});
            for (std::size_t i = 0; i < channels.size(); ++i) {
                near(point.outgoing[i], expected[i]);
                if (channels[i].port == 1) {
                    const auto found = old.amplitudes.find(channels[i].bin);
                    near(point.outgoing[i],
                         found == old.amplitudes.end() ? Complex{} : found->second);
                }
            }
            for (std::size_t column = 0; column < 3; ++column) {
                for (auto basis : {Complex{1., 0.}, Complex{0., 1.}}) {
                    if (!channels[column].bin && basis.imag() != 0.) {
                        continue;
                    }
                    std::vector<Complex> direction(channels.size());
                    direction[column] = basis;
                    const auto derivative =
                        sampled_response(channels, waves, coefficients, reference, direction);
                    auto plus = waves, minus = waves;
                    plus[column] += 1e-7 * basis;
                    minus[column] -= 1e-7 * basis;
                    const auto high = linearize_polynomial_amplifier(
                        1., channels, plus, polynomial, 0, 1, reference);
                    const auto low = linearize_polynomial_amplifier(
                        1., channels, minus, polynomial, 0, 1, reference);
                    for (std::size_t row = 0; row < channels.size(); ++row) {
                        const auto actual =
                            point.jacobian.direct()(row, column) * basis +
                            point.jacobian.conjugate()(row, column) * std::conj(basis);
                        near(actual, derivative[row]);
                        require(std::abs(actual - (high.outgoing[row] - low.outgoing[row]) / 2e-7) <
                                    2e-8,
                                "polynomial finite-difference derivative differs");
                    }
                }
            }
            for (std::size_t column = 3; column < channels.size(); ++column) {
                for (std::size_t row = 0; row < channels.size(); ++row) {
                    near(point.jacobian.direct()(row, column), 0.);
                    near(point.jacobian.conjugate()(row, column), 0.);
                }
            }
        }
    }
}

void dc_and_harmonic_noise() {
    const double coefficient = .4, reference = 50., density = 1e-9;
    const Complex amplitude{.03, .04};
    const auto point = linearize_polynomial_amplifier(1.,
                                                      {{0, 1}, {1, 0}, {1, 2}},
                                                      {amplitude, 0., 0.},
                                                      MemorylessPolynomial({0., 0., coefficient}));
    near(point.outgoing[1], coefficient * std::sqrt(reference) * std::norm(amplitude));
    near(point.outgoing[2], coefficient * std::sqrt(reference / 2.) * amplitude * amplitude);
    auto source_noise = point.jacobian.zero_noise();
    source_noise.covariance(0, 0) = density;
    const auto result = point.jacobian.analyze(
        {0., 0., 0.}, {0., 0., 0.}, source_noise, point.jacobian.zero_noise());
    const double noise = 2. * coefficient * coefficient * reference * std::norm(amplitude);
    near(result.outgoing_noise.covariance(1, 1) / density, noise);
    near(result.outgoing_noise.complementary(1, 1) / density, noise);
    near(result.outgoing_noise.covariance(2, 2) / density, noise);
    near(result.outgoing_noise.complementary(2, 2) / density, 0.);
    const auto correlation =
        std::sqrt(2.) * coefficient * coefficient * reference * amplitude * amplitude;
    near(result.outgoing_noise.covariance(1, 2) / density, std::conj(correlation));
    near(result.outgoing_noise.complementary(1, 2) / density, correlation);
    const auto phase_direction = Complex{0., 1.} * amplitude;
    for (std::size_t row = 1; row < 3; ++row) {
        const auto derivative = point.jacobian.direct()(row, 0) * phase_direction +
                                point.jacobian.conjugate()(row, 0) * std::conj(phase_direction);
        near(derivative, row == 1 ? Complex{} : Complex{0., 2.} * point.outgoing[row]);
    }
}

void unoccupied_channel_noise() {
    const MemorylessPolynomial polynomial({0., 0., 0., .1});
    std::vector<ConversionChannel> channels{{0, 2}, {0, 5}};
    std::map<int, std::size_t> outputs;
    for (auto bin : polynomial_output_bins({2, 5}, polynomial)) {
        outputs[bin] = channels.size();
        channels.push_back({1, bin});
    }
    std::vector<Complex> waves(channels.size());
    const Complex pump{.02, .03};
    waves[1] = pump;
    const auto point = linearize_polynomial_amplifier(1., channels, waves, polynomial);
    const auto sum = outputs.at(12), difference = outputs.at(8);
    const auto coupling = 1.5 * .1 * 50. * pump * pump;
    near(point.outgoing[sum], 0.);
    near(point.outgoing[difference], 0.);
    near(point.jacobian.direct()(sum, 0), coupling);
    near(point.jacobian.conjugate()(difference, 0), coupling);
    auto noise = point.jacobian.zero_noise();
    noise.covariance(0, 0) = 1e-9;
    const auto result = point.jacobian.analyze(std::vector<Complex>(channels.size()),
                                               std::vector<Complex>(channels.size()),
                                               noise,
                                               point.jacobian.zero_noise());
    near(result.outgoing_noise.covariance(sum, sum) / 1e-9, std::norm(coupling));
    near(result.outgoing_noise.covariance(difference, difference) / 1e-9, std::norm(coupling));
    near(result.outgoing_noise.covariance(sum, difference) / 1e-9, 0.);
    near(result.outgoing_noise.complementary(sum, difference) / 1e-9, coupling * coupling);
    const auto zero = linearize_polynomial_amplifier(
        1., channels, std::vector<Complex>(channels.size()), polynomial);
    for (const auto &value : zero.jacobian.direct().values) {
        near(value, 0.);
    }
    for (const auto &value : zero.jacobian.conjugate().values) {
        near(value, 0.);
    }
}

ConversionDevice empty_device(const std::vector<ConversionChannel> &channels) {
    const auto n = channels.size();
    const auto zero = conversion_detail::zero(n);
    FrequencyConversionModel base(1., channels, zero, zero);
    return {base,
            std::vector<Complex>(n),
            std::vector<Complex>(n),
            base.zero_noise(),
            base.zero_noise()};
}

void harmonic_feedback() {
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}, {1, 3}};
    auto amplifier = empty_device(channels);
    auto feedback = empty_device({{0, 1}, {0, 3}, {1, 1}});
    auto transfer = conversion_detail::zero(3);
    constexpr double beta = -.2, source = .2, density = 1e-9;
    transfer(2, 0) = beta;
    feedback.model = FrequencyConversionModel(
        1., feedback.model.channels(), transfer, conversion_detail::zero(3));
    feedback.intrinsic_noise.covariance(2, 2) = density;
    const MemorylessPolynomial polynomial({0., 2., 0., -.02});
    ConversionNonlinearDevice law{0, [=](const std::vector<Complex> &waves) {
                                      return linearize_polynomial_amplifier(
                                          1., channels, waves, polynomial);
                                  }};
    const auto result = solve_conversion_operating_point({amplifier, feedback},
                                                         {{0, 1, 1, 0}, {0, 0, 1, 1}},
                                                         {law},
                                                         {},
                                                         {},
                                                         true,
                                                         nullptr,
                                                         {0., 0., 0., 0., 0., source});
    double low = 0., high = source;
    for (int i = 0; i < 80; ++i) {
        const double mid = .5 * (low + high);
        if (mid - beta * (2. * mid - 1.5 * mid * mid * mid) > source) {
            high = mid;
        } else {
            low = mid;
        }
    }
    const double a = .5 * (low + high);
    require(std::abs(result.waves.incident[0] - a) < 1e-10, "polynomial feedback root differs");
    require(std::abs(result.waves.outgoing[1] - (2. * a - 1.5 * a * a * a)) < 1e-10,
            "feedback fundamental differs");
    require(std::abs(result.waves.outgoing[2] + .5 * a * a * a) < 1e-10,
            "feedback harmonic differs");
    const double radial = 2. - 4.5 * a * a, tangent = 2. - 1.5 * a * a, harmonic = -1.5 * a * a;
    const double rr = radial / (1. - beta * radial), ri = tangent / (1. - beta * tangent);
    const double hr = harmonic / (1. - beta * radial), hi = harmonic / (1. - beta * tangent);
    near(result.waves.outgoing_noise.covariance(1, 1) / density, .5 * (rr * rr + ri * ri));
    near(result.waves.outgoing_noise.complementary(1, 1) / density, .5 * (rr * rr - ri * ri));
    near(result.waves.outgoing_noise.covariance(2, 2) / density, .5 * (hr * hr + hi * hi));
    near(result.waves.outgoing_noise.covariance(1, 2) / density, .5 * (rr * hr + ri * hi));
    near(result.waves.outgoing_noise.complementary(1, 2) / density, .5 * (rr * hr - ri * hi));
    require(result.scaled_residual <= 1. && result.iterations > 1,
            "feedback solve did not iterate");
}

void boundary_contracts() {
    const MemorylessPolynomial constant({2.}), linear({0., -2.});
    const std::vector<ConversionChannel> channels{{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    const std::vector<Complex> waves{.1, {.03, .04}, .2, {.5, .6}};
    const auto c = linearize_polynomial_amplifier(1., channels, waves, constant);
    near(c.outgoing[2], 2. / std::sqrt(50.));
    near(c.outgoing[3], 0.);
    const auto l = linearize_polynomial_amplifier(1., channels, waves, linear);
    near(l.outgoing[2], -.2);
    near(l.outgoing[3], -2. * waves[1]);
    near(l.jacobian.direct()(2, 0), -1.);
    near(l.jacobian.conjugate()(2, 0), -1.);
    for (auto index : {0, 2}) {
        auto bad = waves;
        bad[index] += Complex{0., 1.};
        rejects<std::invalid_argument>([&] {
            linearize_polynomial_amplifier(1., channels, bad, linear);
        });
    }
    rejects<std::invalid_argument>([&] {
        linearize_polynomial_amplifier(1., channels, waves, linear, 0, 0);
    });
    rejects<std::invalid_argument>([&] {
        linearize_polynomial_amplifier(1., channels, waves, linear, 0, 2);
    });
    rejects<std::invalid_argument>([&] {
        linearize_polynomial_amplifier(1., channels, waves, linear, 0, 1, 0.);
    });
    rejects<std::invalid_argument>([&] {
        linearize_polynomial_amplifier(1., channels, {}, linear);
    });
    auto bad = waves;
    bad[1] = std::numeric_limits<double>::infinity();
    rejects<std::invalid_argument>([&] {
        linearize_polynomial_amplifier(1., channels, bad, linear);
    });
}

int main() {
    try {
        structural_support();
        sampled_values_and_derivatives();
        dc_and_harmonic_noise();
        unoccupied_channel_noise();
        harmonic_feedback();
        boundary_contracts();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
