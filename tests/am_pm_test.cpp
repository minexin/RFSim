#include "rfmodel/am_pm_linearization.hpp"
#include "rfmodel/amplifier_linearization.hpp"
#include "rfmodel/highorder_linearization.hpp"
#include <iostream>
#include <limits>

using namespace rfmodel;

void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void near(Complex a, Complex b, double tolerance = 1e-10) {
    if (std::abs(a - b) > tolerance * std::max(1., std::abs(b))) {
        std::cerr << a << " != " << b << '\n';
        throw std::runtime_error("AM-PM mismatch");
    }
}

template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    require(caught, "invalid input accepted");
}

// Integrate the specified dB-domain derivative independently of the closed form.
double integrated_phase(double power, double peak = -5., double reference = 10.) {
    if (power == 0.) {
        return 0.;
    }
    const double end = std::min(10. * std::log10(power) + 30. - reference, 20. * std::log10(1.5));
    if (end <= -160.) {
        return 0.;
    }
    const int n = 12000;
    const double step = (end + 160.) / n;
    double sum = 0.;
    for (int i = 0; i <= n; ++i) {
        const double u = std::pow(10., (-160. + step * i) / 20.);
        sum += ((i == 0 || i == n) ? 1. : (i % 2 ? 4. : 2.)) * peak * u * u * (3. - 2. * u);
    }
    return sum * step / 3. * std::acos(-1.) / 180.;
}

void phase_law() {
    const double pi = std::acos(-1.);
    for (double peak : {-5., 0., 8.}) {
        for (double reference : {-40., 10., 50.}) {
            const LegacyPointDerivativeAmPm law(peak, reference);
            const double anchor = std::pow(10., (reference - 30.) / 10.);
            near(law.response(0.).phase_rad, 0.);
            near(law.response(0.).radial_phase_slope, 0.);
            near(law.response(anchor).radial_phase_slope * std::log(10.) / 20. * 180. / pi, peak);
            for (double offset : {-100., -30., -10., -1., 0., 1., 3., 4., 30.}) {
                const double p = anchor * std::pow(10., offset / 10.), h = 1e-5;
                const auto value = law.response(p);
                near(value.phase_rad, integrated_phase(p, peak, reference), 2e-10);
                near(value.radial_phase_slope,
                     (law.response(p * std::exp(2 * h)).phase_rad -
                      law.response(p * std::exp(-2 * h)).phase_rad) /
                         (2 * h),
                     2e-8);
                require(std::abs(value.radial_phase_slope * std::log(10.) / 20. * 180. / pi) <=
                            std::abs(peak) + 1e-12,
                        "peak exceeded");
                near(std::norm(law.transmit(std::polar(std::sqrt(p), .7))) / p, 1.);
            }
            near(law.response(2.25 * anchor).phase_rad, peak * 22.5 / std::log(10.) * pi / 180.);
            near(law.response(2.25 * anchor).radial_phase_slope, 0.);
            const double edge = 2.25 * anchor;
            near(law.response(edge * (1. - 1e-8)).phase_rad,
                 law.response(edge * (1. + 1e-8)).phase_rad);
        }
    }
    near(LegacyPointDerivativeAmPm(-5., 10.).response(1.).phase_rad * 180. / pi,
         -48.85812921486583);
    require(std::isfinite(LegacyPointDerivativeAmPm(5., -3000.).response(1e300).phase_rad),
            "ratio overflow");
}

void composition() {
    const CoherentHighOrderAmplifier amplifier(20., 20., 23., {0., .1});
    const LegacyPointDerivativeAmPm phase(-5., 10.);
    const std::vector<ConversionChannel> channels{{0, 1}, {0, 3}, {1, 1}, {1, 3}};
    const auto base = [&](const std::vector<Complex> &waves) {
        return linearize_highorder_amplifier(1., channels, waves, amplifier, {1});
    };
    for (double drive : {0., .005, .01, .02, .06, .1}) {
        const std::vector<Complex> waves{{drive, .2 * drive}, {0., .3 * drive}, .1, {0., .3}};
        const auto original = base(waves);
        const auto point = apply_output_am_pm(original, phase, {2});
        near(point.outgoing[3], original.outgoing[3]);
        for (std::size_t j = 0; j < 4; ++j) {
            near(point.jacobian.direct()(3, j), original.jacobian.direct()(3, j));
            near(point.jacobian.conjugate()(3, j), original.jacobian.conjugate()(3, j));
            for (Complex h : {Complex(1e-7, 0.), Complex(0., 1e-7)}) {
                auto hi = waves, lo = waves;
                hi[j] += h;
                lo[j] -= h;
                const auto response = [](Complex z) {
                    return std::polar(1., integrated_phase(std::norm(z))) * z;
                };
                near((point.jacobian.direct()(2, j) * h +
                      point.jacobian.conjugate()(2, j) * std::conj(h)) /
                         std::abs(h),
                     (response(base(hi).outgoing[2]) - response(base(lo).outgoing[2])) /
                         (2 * std::abs(h)),
                     4e-8);
            }
        }
        const auto both = apply_output_am_pm(original, phase, {2, 3});
        near(both.outgoing[2], point.outgoing[2]);
        near(both.outgoing[3], phase.transmit(original.outgoing[3]));
        require(apply_output_am_pm(original, phase, {}).outgoing == original.outgoing,
                "empty selection changed output");
        require(apply_output_am_pm(original, LegacyPointDerivativeAmPm(0., 10.), {2, 3}).outgoing ==
                    original.outgoing,
                "zero slope changed output");
    }
}

// Original cubic/tanh amplitude formula, independent of the adapter.
Complex reference_amplifier(Complex a) {
    const double r = std::abs(a), anchor = std::sqrt(std::pow(10., -2.9));
    double m;
    if (r <= anchor) {
        m = 10. * r * (1. - (1. - std::pow(10., -.05)) * std::norm(a) / (anchor * anchor));
    } else {
        const double head = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1);
        m = std::sqrt(.1) +
            head * std::tanh(10. * (3. * std::pow(10., -.05) - 2.) * (r - anchor) / head);
    }
    return r ? a / r * m * std::polar(1., integrated_phase(m * m)) : Complex{};
}

void feedback_noise() {
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}};
    const auto zero = conversion_detail::zero(2);
    FrequencyConversionModel base(1., channels, zero, zero);
    ConversionDevice active{base, {0., 0.}, {0., 0.}, base.zero_noise(), base.zero_noise()};
    auto feedback = active;
    auto transfer = zero;
    transfer(1, 0) = -.03;
    feedback.model = FrequencyConversionModel(1., channels, transfer, zero);
    feedback.intrinsic_noise.covariance(1, 1) = 1e-9;
    const SaturatingFundamentalCompression amplifier(20., 20., 23.);
    const LegacyPointDerivativeAmPm phase(-5., 10.);
    const ConversionNonlinearDevice law{
        0, [=](const std::vector<Complex> &waves) {
            return apply_output_am_pm(
                linearize_saturating_amplifier(1., channels, waves, amplifier), phase, {1});
        }};
    const Complex source(.01, .003);
    const auto solved = solve_conversion_operating_point({active, feedback},
                                                         {{0, 1, 1, 0}, {0, 0, 1, 1}},
                                                         {law},
                                                         {},
                                                         {},
                                                         true,
                                                         nullptr,
                                                         {0., 0., 0., source});
    Complex root = source;
    for (int i = 0; i < 200; ++i) {
        root = source - .03 * reference_amplifier(root);
    }
    near(solved.waves.incident[0], root);
    near(solved.waves.outgoing[1], reference_amplifier(root));
    require(std::abs(phase.response(std::norm(solved.waves.outgoing[1])).radial_phase_slope) > .1,
            "feedback oracle must exercise active AM-PM slope");
    const auto a = solved.waves.incident[0];
    const double h = 1e-7;
    const Complex dx = (reference_amplifier(a + h) - reference_amplifier(a - h)) / (2 * h);
    const Complex dy =
        (reference_amplifier(a + Complex(0., h)) - reference_amplifier(a - Complex(0., h))) /
        (2 * h);
    const double m00 = 1. + .03 * dx.real(), m01 = .03 * dy.real(), m10 = .03 * dx.imag(),
                 m11 = 1. + .03 * dy.imag();
    const double determinant = m00 * m11 - m01 * m10;
    const auto rx = (dx * m11 - dy * m10) / determinant, ry = (-dx * m01 + dy * m00) / determinant;
    near(solved.waves.outgoing_noise.covariance(1, 1) / 1e-9,
         .5 * (std::norm(rx) + std::norm(ry)),
         1e-7);
    near(solved.waves.outgoing_noise.complementary(1, 1) / 1e-9, .5 * (rx * rx + ry * ry), 1e-7);
    require(std::abs(solved.waves.outgoing_noise.complementary(1, 1).imag()) > 1e-10,
            "missing phase noise correlation");
}

void contracts() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const LegacyPointDerivativeAmPm phase(-5., 10.);
    rejects([&] {
        LegacyPointDerivativeAmPm bad(nan, 10.);
    });
    rejects([&] {
        LegacyPointDerivativeAmPm bad(5., nan);
    });
    rejects([&] {
        LegacyPointDerivativeAmPm bad(5., 4000.);
    });
    rejects([&] {
        LegacyPointDerivativeAmPm bad(5., -4000.);
    });
    rejects([&] {
        phase.response(-1.);
    });
    rejects([&] {
        phase.response(nan);
    });
    rejects([&] {
        phase.transmit({nan, 0.});
    });
    rejects([&] {
        phase.transmit({1e300, 0.});
    });
    const auto zero = conversion_detail::zero(2);
    ConversionLinearization point{FrequencyConversionModel(1., {{0, 0}, {1, 1}}, zero, zero),
                                  {0., .1}};
    rejects([&] {
        apply_output_am_pm(point, phase, {0});
    });
    rejects([&] {
        apply_output_am_pm(point, phase, {2});
    });
    rejects([&] {
        apply_output_am_pm(point, phase, {1, 1});
    });
    point.outgoing[0] = {0., .1};
    rejects([&] {
        apply_output_am_pm(point, phase, {1});
    });
    point.outgoing = {0., {nan, 0.}};
    rejects([&] {
        apply_output_am_pm(point, phase, {});
    });
    point.outgoing = {0.};
    rejects([&] {
        apply_output_am_pm(point, phase, {1});
    });
}

int main() {
    try {
        phase_law();
        composition();
        feedback_noise();
        contracts();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
