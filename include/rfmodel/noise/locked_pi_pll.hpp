#pragma once
#include "rfmodel/noise/phase_noise.hpp"
#include <initializer_list>

namespace rfmodel {
// Already-locked, linearized, zero-delay PI loop. These are angular-frequency
// gains: do not supply Hz/V or charge-pump current without converting units.
struct LockedPiPllParameters {
    double detector_a_per_rad{1.};
    double vco_rad_per_s_per_v{1.};
    // Positive real effective ratio; not a fractional-N spur model.
    double feedback_division{1.};
    double resistance_ohms{1.};
    double capacitance_f{1.};
};

struct LockedPiPllNoise {
    double reference_rad2_per_hz{};
    double divider_rad2_per_hz{};
    double vco_rad2_per_hz{};
    double detector_current_a2_per_hz{};
    double tuning_voltage_v2_per_hz{};
};

struct LockedPiPllTransfer {
    Complex reference{}, divider{}, vco{};
    Complex detector_current_rad_per_a{}, tuning_voltage_rad_per_v{};
};

struct LockedPiPllContributions {
    double reference_rad2_per_hz{}, divider_rad2_per_hz{}, vco_rad2_per_hz{};
    double detector_current_rad2_per_hz{}, tuning_voltage_rad2_per_hz{};
    double total_rad2_per_hz{};
};

struct LockedPiPllResult {
    double offset_hz{};
    LockedPiPllTransfer transfer;
    LockedPiPllContributions noise;
};

namespace locked_pi_pll_detail {
// Bounded products of binary64 inputs, represented without intermediate overflow.
// No extended-precision arithmetic, arbitrary-precision dependency, or device law.
struct Scaled {
    double fraction{1.};
    int exponent{};
};

inline Scaled product(std::initializer_list<double> numerator,
                      std::initializer_list<double> denominator = {}) {
    Scaled result;
    auto factor = [&](double value, bool divide) {
        int exponent = 0;
        const double fraction = std::frexp(value, &exponent);
        result.fraction = divide ? result.fraction / fraction : result.fraction * fraction;
        result.exponent += divide ? -exponent : exponent;
        result.fraction = std::frexp(result.fraction, &exponent);
        result.exponent += exponent;
    };
    for (double value : numerator) {
        factor(value, false);
    }
    for (double value : denominator) {
        factor(value, true);
    }
    return result;
}

inline double at_scale(Scaled value, int exponent) {
    return std::scalbn(value.fraction, value.exponent - exponent);
}

inline double materialize(double value, int exponent) {
    if (value == 0.) {
        return 0.;
    }
    const double result = std::scalbn(value, exponent);
    if (!std::isfinite(result) || std::abs(result) < std::numeric_limits<double>::min()) {
        throw std::overflow_error("PLL result component outside finite normal binary64 range");
    }
    return result;
}

inline Complex ratio(Scaled real,
                     Scaled imag,
                     Complex denominator,
                     int denominator_exponent,
                     bool real_only = false) {
    const int exponent = std::max(real.exponent, imag.exponent);
    const double re = at_scale(real, exponent), im = at_scale(imag, exponent);
    if ((real.fraction != 0. && re == 0.) || (imag.fraction != 0. && im == 0.)) {
        throw std::overflow_error("PLL numerator dynamic range exceeds binary64 resolution");
    }
    const Complex value = Complex{re, im} / denominator;
    return {materialize(value.real(), exponent - denominator_exponent),
            real_only ? 0. : materialize(value.imag(), exponent - denominator_exponent)};
}

inline Scaled zero(int exponent) {
    return {0., exponent};
}

inline double contribution(Complex transfer, double density) {
    if (!std::isfinite(density) || density < 0.) {
        throw std::invalid_argument("PLL source PSD must be finite and nonnegative");
    }
    if (density == 0.) {
        return 0.;
    }
    const double scale = std::max(std::abs(transfer.real()), std::abs(transfer.imag()));
    if (scale == 0.) {
        throw std::overflow_error("PLL transfer underflow");
    }
    const auto value = product({scale, scale, density, std::norm(transfer / scale)});
    return materialize(value.fraction, value.exponent);
}
} // namespace locked_pi_pll_detail

// Independent, one-sided source PSDs. The divider source is added after the
// ideal division, so its complex transfer has the negative feedback sign.
inline LockedPiPllResult analyze_locked_pi_pll(const LockedPiPllParameters &p,
                                               double offset_hz,
                                               const LockedPiPllNoise &noise = {}) {
    for (double value : {p.detector_a_per_rad,
                         p.vco_rad_per_s_per_v,
                         p.feedback_division,
                         p.resistance_ohms,
                         p.capacitance_f,
                         offset_hz}) {
        if (!std::isfinite(value) || value <= 0.) {
            throw std::invalid_argument("PLL parameters and offset Hz must be finite and positive");
        }
    }
    using namespace locked_pi_pll_detail;
    const double k = p.detector_a_per_rad, v = p.vco_rad_per_s_per_v;
    const double n = p.feedback_division, r = p.resistance_ohms, c = p.capacitance_f;
    const double tau = 2. * std::acos(-1.);
    // Q(s)=s^2+a*s+b, a=Kpd*Kvco*R/N>0, b=Kpd*Kvco/(N*C)>0.
    // Positivity proves this restricted continuous-time polynomial Hurwitz;
    // it does not demonstrate physical acquisition or lock of a device.
    const auto b = product({k, v}, {n, c});
    const auto aw = product({k, v, r, tau, offset_hz}, {n});
    const auto w2 = product({tau, offset_hz, tau, offset_hz});
    const int exponent = std::max({b.exponent, aw.exponent, w2.exponent});
    const double bs = at_scale(b, exponent), as = at_scale(aw, exponent);
    const double ws = at_scale(w2, exponent);
    const Complex denominator{bs - ws, as};
    // Near undamped resonance, binary64 input/coefficient rounding can dominate
    // Q. Reject instead of interpreting a nonzero sampled denominator as safety.
    if (std::abs(denominator) <= 64. * std::numeric_limits<double>::epsilon() * (bs + ws + as)) {
        throw std::overflow_error("PLL denominator is ill-conditioned at this offset");
    }
    LockedPiPllResult result;
    result.offset_hz = offset_hz;
    auto &h = result.transfer;
    h.reference = ratio(
        product({k, v}, {c}), product({k, v, r, tau, offset_hz}), denominator, exponent, true);
    // Im(Href)=-Kpd*Kvco*R*omega^3/|Q|^2. Computing it as a
    // difference of two nearly equal terms loses the low-frequency phase lag.
    const auto reference_imag = product({k, v, r, tau, offset_hz, tau, offset_hz, tau, offset_hz});
    h.reference.imag(-materialize(reference_imag.fraction / std::norm(denominator),
                                  reference_imag.exponent - 2 * exponent));
    h.divider = -h.reference;
    auto negative_w2 = w2;
    negative_w2.fraction = -negative_w2.fraction;
    h.vco = ratio(negative_w2, zero(w2.exponent), denominator, exponent);
    h.detector_current_rad_per_a =
        ratio(product({v}, {c}), product({v, r, tau, offset_hz}), denominator, exponent, true);
    const auto current_imag = product({v, r, tau, offset_hz, tau, offset_hz, tau, offset_hz});
    h.detector_current_rad_per_a.imag(-materialize(current_imag.fraction / std::norm(denominator),
                                                   current_imag.exponent - 2 * exponent));
    const auto vw = product({v, tau, offset_hz});
    h.tuning_voltage_rad_per_v = ratio(zero(vw.exponent), vw, denominator, exponent);
    auto &out = result.noise;
    out.reference_rad2_per_hz = contribution(h.reference, noise.reference_rad2_per_hz);
    out.divider_rad2_per_hz = contribution(h.divider, noise.divider_rad2_per_hz);
    out.vco_rad2_per_hz = contribution(h.vco, noise.vco_rad2_per_hz);
    out.detector_current_rad2_per_hz =
        contribution(h.detector_current_rad_per_a, noise.detector_current_a2_per_hz);
    out.tuning_voltage_rad2_per_hz =
        contribution(h.tuning_voltage_rad_per_v, noise.tuning_voltage_v2_per_hz);
    out.total_rad2_per_hz = out.reference_rad2_per_hz + out.divider_rad2_per_hz +
                            out.vco_rad2_per_hz + out.detector_current_rad2_per_hz +
                            out.tuning_voltage_rad2_per_hz;
    if (!std::isfinite(out.total_rad2_per_hz)) {
        throw std::overflow_error("PLL total output phase PSD overflow");
    }
    return result;
}

// A single independent output only. This does not preserve cross-output PLL
// correlations; never substitute |H| or a real phase_gain for complex H there.
struct LockedPiPllSidebandPoint {
    int offset_bin{};
    double offset_hz{};
    double total_phase_rad2_per_hz{};
};

inline ConversionNoise
locked_pi_pll_sidebands(const std::vector<ConversionChannel> &channels,
                        std::size_t carrier_channel,
                        Complex carrier_wave,
                        double spacing_hz,
                        const std::vector<LockedPiPllSidebandPoint> &points) {
    if (!std::isfinite(spacing_hz) || spacing_hz <= 0. || points.empty() || points.size() > 255) {
        throw std::invalid_argument("invalid PLL sideband spacing or point count");
    }
    std::vector<PhaseNoiseOffset> shape, active;
    for (const auto &point : points) {
        const double expected = spacing_hz * point.offset_bin;
        if (point.offset_bin <= 0 || !std::isfinite(expected) || expected <= 0. ||
            !std::isfinite(point.offset_hz) || point.offset_hz != expected ||
            !std::isfinite(point.total_phase_rad2_per_hz) || point.total_phase_rad2_per_hz < 0.) {
            throw std::invalid_argument(
                "PLL offset Hz must equal bin times spacing; PSD must be nonnegative");
        }
        shape.push_back({point.offset_bin, 0.});
        if (point.total_phase_rad2_per_hz > 0.) {
            // L(f)=S_phi(f)/2. Log form avoids underflow of the division by 2.
            active.push_back(
                {point.offset_bin,
                 10. / std::log(10.) * (std::log(point.total_phase_rad2_per_hz) - std::log(2.))});
        }
    }
    // Validate even silent points (ordering, channels, carrier, both sidebands).
    auto silent = phase_noise_group(channels, {{carrier_channel, carrier_wave, 0.}}, shape);
    return active.empty() ? silent
                          : phase_noise_sidebands(channels, carrier_channel, carrier_wave, active);
}
} // namespace rfmodel
