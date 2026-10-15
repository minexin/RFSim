#include "rfmodel/noise/locked_pi_pll.hpp"
#include "rfmodel/conversion/conversion_power_wave.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
void relative(Complex actual, Complex expected, double tolerance = 3e-12) {
    require(std::abs(actual - expected) <= tolerance * std::max(1e-300, std::abs(expected)),
            "PLL relative oracle mismatch");
}

void algebra() {
    const double tau = 2. * std::acos(-1.);
    const LockedPiPllParameters p{1., 1., 1., 2., 1.};
    const auto result = analyze_locked_pi_pll(p, 1. / tau, {1., 2., 3., 4., 5.});
    const auto &h = result.transfer;
    near(h.reference, {1., -.5});
    near(h.divider, {-1., .5});
    near(h.vco, {0., .5});
    near(h.detector_current_rad_per_a, {1., -.5});
    near(h.tuning_voltage_rad_per_v, .5);
    const auto &o = result.noise;
    near(o.reference_rad2_per_hz, 1.25);
    near(o.divider_rad2_per_hz, 2.5);
    near(o.vco_rad2_per_hz, .75);
    near(o.detector_current_rad2_per_hz, 5.);
    near(o.tuning_voltage_rad2_per_hz, 1.25);
    near(o.total_rad2_per_hz, 10.75);
    near(analyze_locked_pi_pll(p, 1.).noise.total_rad2_per_hz, 0.);
    const double weights[]{1.25, 1.25, .25, 1.25, .25};
    for (int field = 0; field < 5; ++field) {
        LockedPiPllNoise isolated;
        double *fields[]{&isolated.reference_rad2_per_hz,
                         &isolated.divider_rad2_per_hz,
                         &isolated.vco_rad2_per_hz,
                         &isolated.detector_current_a2_per_hz,
                         &isolated.tuning_voltage_v2_per_hz};
        *fields[field] = 1e-12;
        relative(analyze_locked_pi_pll(p, 1. / tau, isolated).noise.total_rad2_per_hz,
                 weights[field] * 1e-12);
    }

    for (const auto &q : {p, LockedPiPllParameters{.003, 2e7, 17.5, 210., 4e-8}}) {
        for (double f : {1e-5, .1, 1., 1e3, 1e6, 1e10}) {
            const auto t = analyze_locked_pi_pll(q, f).transfer;
            const Complex s{0., tau * f};
            const Complex z = q.resistance_ohms + 1. / (s * q.capacitance_f);
            const Complex g = q.detector_a_per_rad * z * q.vco_rad_per_s_per_v / s;
            const Complex d = 1. + g / q.feedback_division;
            relative(t.reference, g / d);
            relative(t.divider, -g / d);
            relative(t.vco, 1. / d);
            relative(t.detector_current_rad_per_a, z * q.vco_rad_per_s_per_v / (s * d));
            relative(t.tuning_voltage_rad_per_v, q.vco_rad_per_s_per_v / (s * d));
            near(t.reference / q.feedback_division + t.vco, 1.);
        }
    }
    const auto low = analyze_locked_pi_pll({1., 1., 7., 2., 1.}, 1e-9).transfer;
    relative(low.reference, 7., 1e-7);
    relative(low.reference.imag(), -7. * low.vco.imag());
    require(low.reference.imag() < 0., "low-frequency complex lag must not cancel to zero");
    require(std::abs(low.vco) < 1e-14, "VCO low-frequency suppression");
    const auto high = analyze_locked_pi_pll(p, 1e10).transfer;
    require(std::abs(high.reference) < 1e-9, "reference high-frequency suppression");
    relative(high.vco, 1., 1e-9);
}

void domains() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double invalid : {0., -1., nan, inf}) {
        rejects<std::invalid_argument>([&] {
            analyze_locked_pi_pll({}, invalid);
        });
        for (int field = 0; field < 5; ++field) {
            LockedPiPllParameters p;
            double *fields[]{&p.detector_a_per_rad,
                             &p.vco_rad_per_s_per_v,
                             &p.feedback_division,
                             &p.resistance_ohms,
                             &p.capacitance_f};
            *fields[field] = invalid;
            rejects<std::invalid_argument>([&] {
                analyze_locked_pi_pll(p, 1.);
            });
        }
    }
    for (double invalid : {-1., nan, inf}) {
        for (int field = 0; field < 5; ++field) {
            LockedPiPllNoise n;
            double *fields[]{&n.reference_rad2_per_hz,
                             &n.divider_rad2_per_hz,
                             &n.vco_rad2_per_hz,
                             &n.detector_current_a2_per_hz,
                             &n.tuning_voltage_v2_per_hz};
            *fields[field] = invalid;
            rejects<std::invalid_argument>([&] {
                analyze_locked_pi_pll({}, 1., n);
            });
        }
    }
    const double f = 1. / (2. * std::acos(-1.));
    rejects<std::overflow_error>([&] {
        analyze_locked_pi_pll({1., 1., 1., 1e-16, 1.}, f);
    });
    const auto sharp = analyze_locked_pi_pll({1., 1., 1., 1e-10, 1.}, f);
    relative(sharp.transfer.vco, Complex{0., 1e10}, 1e-6);
    // Products k*v overflow/underflow, but the polynomial and final transfers do not.
    for (double scale : {1e200, 1e-200}) {
        const auto h = analyze_locked_pi_pll({scale, scale, scale, 2. / scale, scale}, f).transfer;
        relative(h.reference, scale * Complex{1., -.5});
        relative(h.vco, Complex{0., .5});
        relative(h.detector_current_rad_per_a, Complex{1., -.5});
        relative(h.tuning_voltage_rad_per_v, .5 * scale);
    }
    const auto weighted =
        analyze_locked_pi_pll({1e200, 1e200, 1e200, 2e-200, 1e200}, f, {1e-200, 0., 0., 0., 0.});
    relative(weighted.noise.total_rad2_per_hz, 1.25e200);
    rejects<std::overflow_error>([&] {
        analyze_locked_pi_pll({1e200, 1e200, 1e200, 2e-200, 1e200}, f, {1., 0., 0., 0., 0.});
    });
    rejects<std::overflow_error>([&] {
        analyze_locked_pi_pll({1., 1., 1., 2., 1.}, f, {1e308, 1e308, 0., 0., 0.});
    });
    rejects<std::overflow_error>([&] {
        analyze_locked_pi_pll({}, 1e-200);
    });
}

void sidebands() {
    const std::vector<ConversionChannel> channels{{0, 9}, {0, 10}, {0, 11}};
    const auto noise = locked_pi_pll_sidebands(channels, 1, {1., 2.}, 3., {{1, 3., 2e-10}});
    relative(noise.covariance(0, 0), 5e-10);
    relative(noise.covariance(2, 2), 5e-10);
    relative(noise.complementary(0, 2), Complex{3e-10, -4e-10});
    near(noise.covariance(0, 2), 0.);
    const auto silent = locked_pi_pll_sidebands(channels, 1, 1., 3., {{1, 3., 0.}});
    near(silent.covariance(0, 0), 0.);
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands(channels, 1, 1., 3., {{1, 3.1, 0.}});
    });
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands(channels, 1, 1., 3., {{1, 3., -1.}});
    });
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands(channels, 1, 1., 3., {{1, 3., 0.}, {1, 3., 0.}});
    });
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands({{0, 10}}, 0, 1., 3., {{1, 3., 0.}});
    });
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands(channels, 1, 0., 3., {{1, 3., 0.}});
    });
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands(channels, 1, 1., 3., {{10, 30., 0.}});
    });
    for (double invalid : {0.,
                           -1.,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        rejects<std::invalid_argument>([&] {
            locked_pi_pll_sidebands(channels, 1, 1., invalid, {{1, 3., 0.}});
        });
    }
    rejects<std::overflow_error>([&] {
        locked_pi_pll_sidebands(channels, 1, 1e-200, 3., {{1, 3., 1e-20}});
    });
    rejects<std::overflow_error>([&] {
        locked_pi_pll_sidebands(channels, 1, 1e200, 3., {{1, 3., 1e20}});
    });
    const auto next_offset = std::nextafter(3., 4.);
    rejects<std::invalid_argument>([&] {
        locked_pi_pll_sidebands(channels, 1, 1., 3., {{1, next_offset, 0.}});
    });
    // Existing boundary renormalization owns the reference conversion. Verify a
    // complex-reference round trip of C and P without treating rad^2/Hz as W/Hz.
    const auto zero = conversion_detail::zero(3);
    const FrequencyConversionModel original(3., channels, zero, zero);
    const std::vector<Complex> refs{{37., 11.}, {75., -8.}, {28., 4.}};
    const std::vector<Complex> source{0., {1., 2.}, 0.}, reflection(3);
    const auto changed =
        renormalize_conversion_boundary(original, refs, {0, 1, 2}, source, reflection, noise);
    const auto model = renormalize_power_waves(original, refs).model;
    const auto restored = renormalize_conversion_boundary(model,
                                                          std::vector<Complex>(3, 50.),
                                                          {0, 1, 2},
                                                          changed.source,
                                                          changed.reflection,
                                                          changed.source_noise);
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            relative(restored.source_noise.covariance(i, j), noise.covariance(i, j));
            relative(restored.source_noise.complementary(i, j), noise.complementary(i, j));
        }
    }
}
} // namespace

int main() {
    try {
        algebra();
        domains();
        sidebands();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
