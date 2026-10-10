#include "rfmodel/highorder_linearization.hpp"
#include "test_support.hpp"
#include <iostream>

using namespace rfmodel;

std::vector<Complex> legacy_sum(const std::vector<ConversionChannel> &channels,
                                const std::vector<Complex> &wave,
                                const CoherentHighOrderAmplifier &model,
                                const std::set<int> &generating) {
    std::vector<CoherentComponent> input;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].port == 0 && channels[i].bin > 0) {
            input.push_back(
                {channels[i].bin,
                 generating.count(channels[i].bin) ? SpectrumKind::source : SpectrumKind::harmonic,
                 .001,
                 i + 1,
                 wave[i]});
        }
    }
    const auto response = model.evaluate(1., input, 1000, true);
    std::map<int, Complex> spectrum;
    for (const auto &term : response.terms) {
        spectrum[term.component.bin] += term.component.amplitude;
    }
    std::vector<Complex> result(channels.size());
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].port == 1) {
            result[i] = spectrum[channels[i].bin];
        }
    }
    return result;
}

void limiter_derivative() {
    const double knee = .02, headroom = .04;
    const SoftInputLimiter limiter(knee * knee, (knee + headroom) * (knee + headroom));
    for (auto radius : {0., .01, knee, .025, .1, .5}) {
        const auto value = limiter.gain_response(radius * radius);
        near(limiter.limit(radius), radius * value.amplitude_gain);
        const double step = 1e-8;
        const double fd =
            (limiter.limit(radius + step).real() - limiter.limit(radius - step).real()) /
            (2 * step);
        require(std::abs(fd - value.radial_amplitude_slope) < 2e-8, "limiter radial slope differs");
    }
    const double radius = knee + 20 * headroom;
    const auto deep = limiter.gain_response(radius * radius);
    require(deep.radial_amplitude_slope > 0., "limiter saturated derivative rounded away");
    near(deep.radial_amplitude_slope / (4 * std::exp(-40.)), 1.);
    near(limiter.gain_response(0.).radial_gain_difference, 0.);
    require(std::isfinite(limiter.gain_response(1e300).amplitude_gain),
            "extreme limiter gain overflow");
    rejects<std::invalid_argument>([&] {
        limiter.gain_response(-1.);
    });
}

void legacy_parity_and_chain_derivatives() {
    // One generating input and one propagated distortion: both drive limiting.
    for (int degree : {2, 3, 5, 11}) {
        std::vector<double> coefficients(degree - 1);
        coefficients.back() = .02;
        const CoherentHighOrderAmplifier model(20., 20., 23., coefficients, 75.);
        std::vector<ConversionChannel> channels{{0, 1}, {0, 3}, {1, 0}};
        for (int b = 1; b <= 3 * degree; ++b) {
            channels.push_back({1, b});
        }
        for (const auto &generating :
             {std::vector<int>{1}, std::vector<int>{1, 3}, std::vector<int>{}}) {
            const std::set<int> selected(generating.begin(), generating.end());
            const double knee = std::sqrt(std::pow(10., -3.4));
            const double p1 = std::sqrt(std::pow(10., -2.9));
            for (double radius : {0., .5 * knee, knee, 1.5 * knee, p1, 3 * p1}) {
                std::vector<Complex> waves(channels.size());
                waves[0] = radius * Complex{.8, 0.};
                waves[1] = radius * Complex{0., .6};
                waves[3] = {.01, .02}; // Output incidence must not change drive.
                const auto point =
                    linearize_highorder_amplifier(1., channels, waves, model, generating);
                const auto old = legacy_sum(channels, waves, model, selected);
                for (std::size_t row = 0; row < channels.size(); ++row) {
                    near(point.outgoing[row], old[row]);
                }
                for (std::size_t col = 0; col < 2; ++col) {
                    for (auto direction : {Complex{1., 0.}, Complex{0., 1.}}) {
                        const double step = 1e-8;
                        auto hi = waves, lo = waves;
                        hi[col] += step * direction;
                        lo[col] -= step * direction;
                        const auto upper = legacy_sum(channels, hi, model, selected);
                        const auto lower = legacy_sum(channels, lo, model, selected);
                        for (std::size_t row = 0; row < channels.size(); ++row) {
                            const auto expected = (upper[row] - lower[row]) / (2 * step);
                            const auto actual =
                                point.jacobian.direct()(row, col) * direction +
                                point.jacobian.conjugate()(row, col) * std::conj(direction);
                            require(std::abs(expected - actual) < 3e-6,
                                    "common-limiter chain derivative differs");
                        }
                    }
                }
                for (std::size_t row = 0; row < channels.size(); ++row) {
                    near(point.jacobian.direct()(row, 3), 0.);
                    near(point.jacobian.conjugate()(row, 3), 0.);
                    // Time translation rotates every generated product by its own frequency.
                    Complex variation{};
                    for (std::size_t col = 0; col < 2; ++col) {
                        const auto direction = Complex{0., double(channels[col].bin)} * waves[col];
                        variation += point.jacobian.direct()(row, col) * direction +
                                     point.jacobian.conjugate()(row, col) * std::conj(direction);
                    }
                    near(variation, Complex{0., double(channels[row].bin)} * point.outgoing[row]);
                }
            }
        }
    }
}

void distortion_drive_noise() {
    const CoherentHighOrderAmplifier model(20., 20., 23., {0., .1});
    const std::vector<ConversionChannel> channels{{0, 1}, {0, 5}, {1, 1}, {1, 3}, {1, 5}};
    const std::vector<Complex> waves{.03, Complex{.02, .04}, 0., 0., 0.};
    const auto point = linearize_highorder_amplifier(1., channels, waves, model, {1});
    require(std::abs(point.jacobian.direct()(3, 1)) > 1e-6,
            "propagated distortion must perturb the generated harmonic via shared limiting");
    auto noise = point.jacobian.zero_noise();
    noise.covariance(1, 1) = 1e-9;
    const auto result = point.jacobian.analyze(
        std::vector<Complex>(5), std::vector<Complex>(5), noise, point.jacobian.zero_noise());
    std::vector<Complex> dx(5), dy(5);
    for (auto direction : {Complex{1., 0.}, Complex{0., 1.}}) {
        auto hi = waves, lo = waves;
        hi[1] += 1e-7 * direction;
        lo[1] -= 1e-7 * direction;
        const auto plus = legacy_sum(channels, hi, model, {1}),
                   minus = legacy_sum(channels, lo, model, {1});
        auto &target = direction.imag() == 0. ? dx : dy;
        for (std::size_t i = 0; i < 5; ++i) {
            target[i] = (plus[i] - minus[i]) / 2e-7;
        }
    }
    for (std::size_t i = 2; i < 5; ++i) {
        for (std::size_t j = 2; j < 5; ++j) {
            require(std::abs(result.outgoing_noise.covariance(i, j) / 1e-9 -
                             .5 * (dx[i] * std::conj(dx[j]) + dy[i] * std::conj(dy[j]))) < 1e-7,
                    "high-order covariance differs");
            require(std::abs(result.outgoing_noise.complementary(i, j) / 1e-9 -
                             .5 * (dx[i] * dx[j] + dy[i] * dy[j])) < 1e-7,
                    "high-order complementary differs");
        }
    }
}

// Independent scalar construction of the compression and limiter laws.
struct ScalarResponse {
    double fundamental, harmonic, radial, tangent, harmonic_radial, harmonic_tangent;
};

ScalarResponse response(double a) {
    const double q = std::pow(10., -.05), p1 = std::sqrt(std::pow(10., -2.9));
    const double head = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1), slope = 10 * (3 * q - 2);
    double fundamental, radial;
    if (a <= p1) {
        fundamental = 10 * a * (1 - (1 - q) * (a / p1) * (a / p1));
        radial = 10 * (1 - 3 * (1 - q) * (a / p1) * (a / p1));
    } else {
        const double u = slope * (a - p1) / head;
        fundamental = std::sqrt(.1) + head * std::tanh(u);
        radial = slope / std::pow(std::cosh(u), 2);
    }
    const double knee = std::sqrt(std::pow(10., -3.4)), limit = std::sqrt(std::pow(10., -2.8));
    double l = a, dl = 1.;
    if (a > knee) {
        const double u = (a - knee) / (limit - knee);
        l = knee + (limit - knee) * std::tanh(u);
        dl = 1 / std::pow(std::cosh(u), 2);
    }
    // c3=.1, R=50: cubic fundamental=7.5*l^3, harmonic=2.5*l^3.
    const double total = fundamental + 7.5 * l * l * l, h = 2.5 * l * l * l;
    return {total,
            h,
            radial + 22.5 * l * l * dl,
            a ? total / a : 10.,
            7.5 * l * l * dl,
            a ? 3 * h / a : 0.};
}

void feedback() {
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}, {1, 3}};
    const CoherentHighOrderAmplifier model(20., 20., 23., {0., .1});
    const auto zero = conversion_detail::zero(3);
    FrequencyConversionModel base(1., channels, zero, zero);
    ConversionDevice active{base, {0., 0., 0.}, {0., 0., 0.}, base.zero_noise(), base.zero_noise()};
    auto feedback = active;
    auto transfer = zero;
    transfer(2, 0) = -.2;
    feedback.model = FrequencyConversionModel(1., {{0, 1}, {0, 3}, {1, 1}}, transfer, zero);
    feedback.intrinsic_noise.covariance(2, 2) = 1e-9;
    ConversionNonlinearDevice law{0, [=](const std::vector<Complex> &wave) {
                                      return linearize_highorder_amplifier(
                                          1., channels, wave, model, {1});
                                  }};
    const auto solved = solve_conversion_operating_point({active, feedback},
                                                         {{0, 1, 1, 0}, {0, 0, 1, 1}},
                                                         {law},
                                                         {},
                                                         {},
                                                         true,
                                                         nullptr,
                                                         {0., 0., 0., 0., 0., .2});
    double lo = 0., hi = .2;
    for (int i = 0; i < 80; ++i) {
        const double mid = .5 * (lo + hi);
        if (mid + .2 * response(mid).fundamental > .2) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const double a = .5 * (lo + hi);
    const auto value = response(a);
    require(std::abs(solved.waves.incident[0] - a) < 1e-10, "limited feedback root differs");
    require(std::abs(solved.waves.outgoing[1] - value.fundamental) < 1e-10,
            "limited fundamental differs");
    require(std::abs(solved.waves.outgoing[2] - value.harmonic) < 1e-10,
            "limited harmonic differs");
    // Compare derivatives at the accepted point; the bisection root was checked above.
    const auto noise_point = response(solved.waves.incident[0].real());
    const double rr = noise_point.radial / (1 + .2 * noise_point.radial),
                 ri = noise_point.tangent / (1 + .2 * noise_point.tangent);
    const double hr = noise_point.harmonic_radial / (1 + .2 * noise_point.radial),
                 hi_noise = noise_point.harmonic_tangent / (1 + .2 * noise_point.tangent);
    near(solved.waves.outgoing_noise.covariance(1, 1) / 1e-9, .5 * (rr * rr + ri * ri));
    near(solved.waves.outgoing_noise.covariance(2, 2) / 1e-9, .5 * (hr * hr + hi_noise * hi_noise));
    near(solved.waves.outgoing_noise.complementary(2, 2) / 1e-9,
         .5 * (hr * hr - hi_noise * hi_noise));
    near(solved.waves.outgoing_noise.covariance(1, 2) / 1e-9, .5 * (rr * hr + ri * hi_noise));
}

void contracts() {
    const CoherentHighOrderAmplifier model(20., 20., 23., {.1, .2});
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 0}, {1, 1}, {1, 2}, {1, 3}};
    const auto point =
        linearize_highorder_amplifier(1., channels, {.01, 0., 0., 0., 0.}, model, {1});
    near(point.outgoing[1], 0.);
    for (const auto &bins : {std::vector<int>{0}, std::vector<int>{1, 1}, std::vector<int>{2}}) {
        rejects<std::invalid_argument>([&] {
            linearize_highorder_amplifier(1., channels, {.01, 0., 0., 0., 0.}, model, bins);
        });
    }
    rejects<std::invalid_argument>([&] {
        linearize_highorder_amplifier(1., channels, {.01, .01, 0., 0., 0.}, model, {1});
    });
    rejects<std::invalid_argument>([&] {
        linearize_highorder_amplifier(1., {{0, 1}, {1, 1}}, {0., 0.}, model, {1});
    });
    rejects<std::invalid_argument>([&] {
        linearize_highorder_amplifier(1., {{0, 1}, {1, 2}}, {0., 0.}, model, {});
    });
    const CoherentHighOrderAmplifier no_products(20., 20., 23., {});
    const auto direct =
        linearize_highorder_amplifier(1., {{0, 1}, {1, 1}}, {.1, 0.}, no_products, {1});
    near(direct.outgoing[1], .1 * no_products.operating_point(.01).fundamental_amplitude_gain);
    const auto extreme = linearize_highorder_amplifier(
        1., {{0, 1}, {1, 1}, {1, 2}, {1, 3}}, {1e100, 0., 0., 0.}, model, {1});
    for (auto value : extreme.outgoing) {
        require(std::isfinite(std::abs(value)), "extreme high-order output overflow");
    }
}

int main() {
    try {
        limiter_derivative();
        legacy_parity_and_chain_derivatives();
        distortion_drive_noise();
        feedback();
        contracts();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
