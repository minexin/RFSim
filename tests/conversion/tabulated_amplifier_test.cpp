#include "rfmodel/conversion/tabulated_amplifier_linearization.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <iostream>
#include <limits>

using namespace rfmodel;

namespace {
void close(Complex actual, Complex expected, double scale = 1.) {
    require(std::abs(actual - expected) <= 3e-10 * scale, "scaled numerical mismatch");
}

TabulatedAmplifier analytic(double phase = .2) {
    std::vector<TabulatedAmplifierNode> nodes;
    for (double r : {0., .5, 1., 2.}) {
        nodes.push_back({r, 2. * r - .2 * r * r * r, phase + .4 * r * r, 2. - .6 * r * r, .8 * r});
    }
    return TabulatedAmplifier(nodes);
}

Complex oracle(Complex a, double radius) {
    return a * (2. - .2 * radius * radius) * std::polar(1., .2 + .4 * radius * radius);
}

void response_contract() {
    auto model = analytic();
    for (double r : {0., 1e-12, .13, .5, .8, 1., 1.7, 2.}) {
        auto q = model.response(r);
        close(q.amplitude_sqrt_w, 2. * r - .2 * r * r * r);
        close(q.phase_rad, .2 + .4 * r * r);
        close(q.amplitude_slope, 2. - .6 * r * r);
        close(q.phase_slope, .8 * r);
        close(q.gain, (2. - .2 * r * r) * std::polar(1., .2 + .4 * r * r));
        close(q.radial_gain_difference,
              std::polar(1., .2 + .4 * r * r) *
                  Complex(-.4 * r * r, (2. * r - .2 * r * r * r) * .8 * r));
        close(model.transmit(Complex(.6, .8) * r), oracle(Complex(.6, .8) * r, r));
    }
    for (double knot : {.5, 1.}) {
        const double h = 1e-7;
        close((model.response(knot + h).amplitude_sqrt_w -
               model.response(knot - h).amplitude_sqrt_w) /
                  (2. * h),
              2. - .6 * knot * knot,
              20.);
    }
    // These explicit hand polynomials are different on each side of the knot.
    TabulatedAmplifier piece(
        {{0., 0., 3., 1., .4}, {1., 1.3, 3.4, 1.4, .4}, {2., 2.4, 3.8, .9, .4}});
    for (double r : {.2, .7, 1., 1.3, 1.9}) {
        const double t = r - 1.;
        const double amplitude =
            r <= 1. ? r + .5 * r * r - .2 * r * r * r : 1.3 + 1.4 * t - .4 * t * t + .1 * t * t * t;
        const double slope = r <= 1. ? 1. + r - .6 * r * r : 1.4 - .8 * t + .3 * t * t;
        close(piece.response(r).amplitude_sqrt_w, amplitude);
        close(piece.response(r).amplitude_slope, slope);
        close(piece.response(r).phase_rad, 3. + .4 * r);
        close(piece.transmit(r), std::polar(amplitude, 3. + .4 * r));
    }
    rejects<std::out_of_range>([&] {
        model.response(std::nextafter(2., 3.));
    });
    rejects<std::invalid_argument>([&] {
        model.response(-1.);
    });
    rejects<std::invalid_argument>([&] {
        model.response(std::numeric_limits<double>::quiet_NaN());
    });
    rejects<std::invalid_argument>([] {
        TabulatedAmplifier m({{0., 0., 0., 1., 0.}});
    });
    rejects<std::invalid_argument>([] {
        TabulatedAmplifier m({{0., 1., 0., 1., 0.}, {1., 1., 0., 1., 0.}});
    });
    rejects<std::invalid_argument>([] {
        TabulatedAmplifier m({{0., 0., 0., -1., 0.}, {1., 1., 0., 1., 0.}});
    });
    rejects<std::invalid_argument>([] {
        TabulatedAmplifier m({{0., 0., 0., 1., 0.}, {0., 1., 0., 1., 0.}});
    });
    // Positive endpoints do not guarantee a nonnegative Hermite segment.
    rejects<std::invalid_argument>([] {
        TabulatedAmplifier m({{0., 0., 0., 1., 0.}, {1., 1., 0., -10., 0.}, {2., 1., 0., 10., 0.}});
    });
}

void numeric_edges() {
    TabulatedAmplifier tiny_amplitude({{0., 0., 0., 0., 1e200}, {1., 1., 1e200, 2., 1e200}});
    close(tiny_amplitude.response(1e-200).radial_gain_difference / 1e-200,
          std::polar(1., 1.) * Complex(1., 1.));
    TabulatedAmplifier tiny_phase_derivative(
        {{0., 0., 0., 1e200, 0.}, {1., 1e200, 1e-300, 1e200, 2e-300}});
    const auto paired = tiny_phase_derivative.response(1e-100);
    close(paired.amplitude_sqrt_w / 1e100, 1.);
    close(paired.phase_rad, 0.);
    close(paired.radial_gain_difference / 2e-300, Complex(0., 1.));
    TabulatedAmplifier extreme_unity({{0., 0., 0., 1., 0.}, {1e100, 1e100, 0., 1., 0.}});
    close(extreme_unity.transmit(1e-300, 1e100) / 1e-300, 1.);
    close(extreme_unity.response(1e-300).gain, 1.);
    close(extreme_unity.response(1e-300).amplitude_sqrt_w / 1e-300, 1.);
    TabulatedAmplifier extreme_coupling({{0., 0., 0., 1., 0.}, {1e100, 1e100, 0., 1., 1e200}});
    const auto coupled = linearize_tabulated_amplifier(
        1., {{0, 1}, {1, 1}, {0, 2}, {1, 2}}, {1e100, 0., 1e-300, 0.}, extreme_coupling);
    close(coupled.jacobian.direct()(1, 2) / 5e-101, Complex(0., 1.));
    close(coupled.jacobian.conjugate()(1, 2) / 5e-101, Complex(0., 1.));
    TabulatedAmplifier tiny_phase({{0., 0., 0., 1., 1e-300}, {1., 1., 1e-300, 1., 1e-300}});
    const auto tiny_result = tiny_phase.response(1e-100);
    close(tiny_result.amplitude_sqrt_w / 1e-100, 1.);
    close(tiny_result.gain, 1.);
    require(tiny_result.phase_rad == 0. && tiny_result.radial_gain_difference == Complex{},
            "unrepresentable final phase/coupling should round to zero");
    // An ordinary internal double root is valid, not a negative overshoot.
    for (double epsilon : {0., 1e-12}) {
        TabulatedAmplifier touching(
            {{0., 0., 0., 1. + epsilon, 0.}, {2., 2. + 2. * epsilon, 0., 5. + epsilon, 0.}});
        close(touching.response(1.).amplitude_sqrt_w, epsilon);
        close(touching.response(1.).amplitude_slope, epsilon);
        for (double r : {.999, 1., 1.001}) {
            require(touching.response(r).amplitude_sqrt_w >= 0.,
                    "valid double root became negative");
            close(touching.response(r).amplitude_sqrt_w, r * ((r - 1.) * (r - 1.) + epsilon));
        }
    }
    rejects<std::invalid_argument>([] {
        constexpr double epsilon = 1e-12;
        TabulatedAmplifier negative(
            {{0., 0., 0., 1. - epsilon, 0.}, {2., 2. - 2. * epsilon, 0., 5. - epsilon, 0.}});
    });
    TabulatedAmplifier large_phase({{0., 0., 1e16, 1., 1.}, {1., 1., 1e16 + 2., 1., 1.}});
    close(large_phase.transmit(.5) / .5, large_phase.response(.5).gain);
    for (double endpoint : {0., 1e-30}) {
        rejects<std::invalid_argument>([&] {
            TabulatedAmplifier m({{0., 0., 0., 1., 0.}, {1., endpoint, 0., 1e-10, 0.}});
        });
    }
    const double tiny = std::numeric_limits<double>::denorm_min();
    TabulatedAmplifier narrow({{0., 0., 0., 1., 0.}, {tiny, tiny, 0., 1., 0.}});
    close(narrow.response(tiny).gain, 1.);
    close(narrow.response(tiny).amplitude_slope, 1.);
    close(narrow.response(tiny).radial_gain_difference, 0.);
    const double adjacent = std::nextafter(1., 2.);
    TabulatedAmplifier narrow_normal(
        {{0., 0., 0., 1., 0.}, {1., 1., 0., 1., 0.}, {adjacent, adjacent, 0., 1., 0.}});
    close(narrow_normal.response(adjacent).gain, 1.);
    for (double bad :
         {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        for (int field = 0; field < 5; ++field) {
            std::vector<TabulatedAmplifierNode> n{{0., 0., 0., 1., 0.}, {1., 1., 0., 1., 0.}};
            if (field == 0) {
                n[1].radius_sqrt_w = bad;
            }
            if (field == 1) {
                n[1].amplitude_sqrt_w = bad;
            }
            if (field == 2) {
                n[1].phase_rad = bad;
            }
            if (field == 3) {
                n[1].amplitude_slope = bad;
            }
            if (field == 4) {
                n[1].phase_slope = bad;
            }
            rejects<std::invalid_argument>([&] {
                TabulatedAmplifier m(n);
            });
        }
    }
    rejects<std::overflow_error>([] {
        TabulatedAmplifier m({{0., 0., 0., 1., 0.}, {1., 1e308, 0., 1., 1e308}});
        m.response(1.);
    });
    auto m = analytic();
    rejects<std::out_of_range>([&] {
        m.transmit(3.);
    });
    rejects<std::out_of_range>([&] {
        linearize_tabulated_amplifier(1., {{0, 1}, {1, 1}}, {3., 0.}, m);
    });
    const auto zero = linearize_tabulated_amplifier(1., {{0, 1}, {1, 1}}, {0., 0.}, m);
    close(zero.jacobian.direct()(1, 0), std::polar(2., .2));
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            close(zero.jacobian.conjugate()(i, j), 0.);
            if (i != 1 || j != 0) {
                close(zero.jacobian.direct()(i, j), 0.);
            }
        }
    }
    for (double knot : {.5, 1.}) {
        for (double side : {-1., 1.}) {
            const auto q = m.response(knot + side * 1e-8);
            close(q.amplitude_slope, 2. - .6 * knot * knot, 100.);
            close(q.phase_slope, .8 * knot, 100.);
        }
    }
}

void conversion_contract() {
    const auto model = analytic();
    const std::vector<ConversionChannel> channels{{0, 3}, {1, 7}, {0, 7}, {1, 3}};
    for (double scale : {0., 1e-4, .5, 1.}) {
        std::vector<Complex> a{
            scale * Complex(.4, .3), Complex(8., -2.), scale * Complex(-.2, .4), Complex(-5., 9.)};
        const double r = std::hypot(std::abs(a[0]), std::abs(a[2]));
        const auto p = linearize_tabulated_amplifier(1e6, channels, a, model);
        close(p.outgoing[0], 0.);
        close(p.outgoing[2], 0.);
        close(p.outgoing[3], oracle(a[0], r));
        close(p.outgoing[1], oracle(a[2], r));
        const double h = 1e-6 * (scale == 0. ? 1e-3 : scale);
        for (std::size_t col = 0; col < 4; ++col) {
            for (Complex direction : {Complex(1., 0.), Complex(0., 1.)}) {
                auto hi = a, lo = a;
                hi[col] += h * direction;
                lo[col] -= h * direction;
                const auto high = linearize_tabulated_amplifier(1e6, channels, hi, model);
                const auto low = linearize_tabulated_amplifier(1e6, channels, lo, model);
                auto f = [&](const std::vector<Complex> &x, std::size_t row) {
                    const double radius = std::hypot(std::abs(x[0]), std::abs(x[2]));
                    return row == 3   ? oracle(x[0], radius)
                           : row == 1 ? oracle(x[2], radius)
                                      : Complex{};
                };
                for (std::size_t row = 0; row < 4; ++row) {
                    const Complex fd = (f(hi, row) - f(lo, row)) / (2. * h);
                    const Complex derivative =
                        p.jacobian.direct()(row, col) * direction +
                        p.jacobian.conjugate()(row, col) * std::conj(direction);
                    require(std::abs(fd - derivative) < 2e-6,
                            "independent Cartesian derivative mismatch");
                    require(std::abs((high.outgoing[row] - low.outgoing[row]) / (2. * h) -
                                     derivative) < 2e-6,
                            "actual Cartesian derivative mismatch");
                }
            }
        }
    }
    // Factory must own both data and channel ordering after the caller goes away.
    auto factory = [] {
        auto m = analytic();
        std::vector<ConversionChannel> c{{1, 3}, {0, 3}};
        return make_tabulated_amplifier_device(9, 1e6, c, m);
    };
    auto owned = factory();
    require(owned.device == 9, "factory lost device index");
    close(owned.evaluate({Complex(99., 0.), Complex(.2, .1)}).outgoing[0],
          oracle(Complex(.2, .1), std::sqrt(.05)));
    auto dc = linearize_tabulated_amplifier(
        1., {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, {0., 0., .1, 0.}, model);
    close(dc.outgoing[0], 0.);
    close(dc.outgoing[1], 0.);
    rejects<std::invalid_argument>([&] {
        linearize_tabulated_amplifier(
            1., {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, {1., 0., .1, 0.}, model);
    });
    rejects<std::invalid_argument>([&] {
        linearize_tabulated_amplifier(1., {{0, 1}, {1, 2}}, {0., 0.}, model);
    });
    rejects<std::invalid_argument>([&] {
        linearize_tabulated_amplifier(1., {{0, 1}, {1, 1}}, {0.}, model);
    });
    rejects<std::invalid_argument>([&] {
        linearize_tabulated_amplifier(1., {{0, 1}, {1, 1}}, {0., 0.}, model, 0, 0);
    });
    rejects<std::invalid_argument>([&] {
        linearize_tabulated_amplifier(1., {{0, 1}, {1, 1}}, {0., 0.}, model, 0, 1, -50.);
    });
    // Norm must not square huge coordinates and overflow when the radius is representable.
    TabulatedAmplifier huge({{0., 0., 0., 1., 0.}, {1e200, 1e200, 0., 1., 0.}});
    const auto large =
        linearize_tabulated_amplifier(1., {{0, 1}, {1, 1}}, {Complex(3e199, 4e199), 0.}, huge);
    close(large.outgoing[1] / 1e199, Complex(3., 4.));
}

void source_and_fixed_offset() {
    const auto amp = analytic();
    const std::vector<ConversionChannel> c{{0, 1}, {1, 1}};
    const auto z = conversion_detail::zero(2);
    FrequencyConversionModel base(1., c, z, z);
    const Complex source(.3, .1), fixed(.07, -.03);
    ConversionDevice active{base, {source, 0.}, {0., 0.}, base.zero_noise(), base.zero_noise()};
    const auto result =
        solve_conversion_operating_point({active},
                                         {},
                                         {make_tabulated_amplifier_device(0, 1., c, amp)},
                                         {},
                                         {},
                                         true,
                                         nullptr,
                                         {0., fixed});
    close(result.waves.incident[0], source);
    close(result.waves.outgoing[1], oracle(source, std::abs(source)) + fixed);
    const auto &m = result.linearized_devices[0].model;
    close(m.direct()(1, 0) * source + m.conjugate()(1, 0) * std::conj(source) +
              result.output_offset[1],
          result.waves.outgoing[1]);
}

void feedback_and_noise() {
    const auto amp = analytic();
    const std::vector<ConversionChannel> c{{0, 1}, {1, 1}};
    const auto z = conversion_detail::zero(2);
    FrequencyConversionModel base(1., c, z, z);
    ConversionDevice active{base, {0., 0.}, {0., 0.}, base.zero_noise(), base.zero_noise()};
    auto feedback = active;
    auto s = z;
    const Complex beta(-.12, .03), root(.4, .2);
    const double r = std::abs(root), density = 1e-9;
    const Complex output = oracle(root, r), source = root - beta * output;
    s(1, 0) = beta;
    feedback.model = FrequencyConversionModel(1., c, s, z);
    feedback.intrinsic_noise.covariance(1, 1) = density;
    auto result = solve_conversion_operating_point({active, feedback},
                                                   {{0, 1, 1, 0}, {0, 0, 1, 1}},
                                                   {make_tabulated_amplifier_device(0, 1., c, amp)},
                                                   {},
                                                   {},
                                                   true,
                                                   nullptr,
                                                   {0., 0., 0., source});
    close(result.waves.incident[0], root, 10.);
    close(result.waves.outgoing[1], output, 10.);
    require(result.scaled_residual <= 1., "feedback residual failed");
    // Independent real Cartesian derivative, then 2x2 feedback inverse.
    const Complex e = std::polar(1., .2 + .4 * r * r);
    const Complex dx =
        e * ((2. - .2 * r * r) + root * Complex(-.4, .8 * (2. - .2 * r * r)) * root.real());
    const Complex dy = e * (Complex(0., 2. - .2 * r * r) +
                            root * Complex(-.4, .8 * (2. - .2 * r * r)) * root.imag());
    const double a = 1. - (beta * dx).real(), b = -(beta * dy).real();
    const double cc = -(beta * dx).imag(), d = 1. - (beta * dy).imag(), det = a * d - b * cc;
    const Complex x = (dx * d - dy * cc) / det, y = (-dx * b + dy * a) / det;
    const auto &accepted = result.linearized_devices[0].model;
    close(accepted.direct()(1, 0), .5 * (dx - Complex(0., 1.) * dy));
    close(accepted.conjugate()(1, 0), .5 * (dx + Complex(0., 1.) * dy));
    const Complex affine = accepted.direct()(1, 0) * result.waves.incident[0] +
                           accepted.conjugate()(1, 0) * std::conj(result.waves.incident[0]) +
                           result.output_offset[1];
    close(affine, result.waves.outgoing[1]);
    close(result.output_offset[3], source);
    close(result.waves.outgoing_noise.covariance(1, 1) / density,
          .5 * (std::norm(x) + std::norm(y)));
    close(result.waves.outgoing_noise.complementary(1, 1) / density, .5 * (x * x + y * y));
}
} // namespace

int main() {
    try {
        response_contract();
        numeric_edges();
        conversion_contract();
        source_and_fixed_offset();
        feedback_and_noise();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
