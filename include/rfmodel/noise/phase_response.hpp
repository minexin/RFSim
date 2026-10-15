#pragma once
#include "rfmodel/noise/phase_noise.hpp"

namespace rfmodel {
// Waves must already use one explicitly chosen positive real power-wave
// generation reference. Use with_phase_response_noise for local complex refs.
struct PhaseResponseCarrier {
    std::size_t channel{};
    Complex wave{};
};

struct PhaseResponsePoint {
    int offset_bin{};
    double offset_hz{};
    double reference_phase_rad2_per_hz{}; // One-sided reference phase PSD.
    std::vector<Complex> member_response; // H(+offset), radians/radian.
};

namespace phase_response_detail {
// Reject unrepresentable nonzero products rather than silently dropping a phase
// component. Exact zero factors and cancellation after representable terms are legal.
inline double product(double a, double b) {
    const double result = a * b;
    if (!std::isfinite(result) || (a != 0. && b != 0. && result == 0.)) {
        throw std::overflow_error("phase response scalar product is not representable");
    }
    return result;
}

inline Complex multiply(Complex a, Complex b) {
    const Complex result{product(a.real(), b.real()) - product(a.imag(), b.imag()),
                         product(a.real(), b.imag()) + product(a.imag(), b.real())};
    if (!conversion_detail::finite(result)) {
        throw std::overflow_error("phase response complex product overflow");
    }
    return result;
}

struct ScaledWave {
    Complex phase;
    double mantissa;
    int exponent;
};

inline ScaledWave decompose(Complex value) {
    const double scale = std::max(std::abs(value.real()), std::abs(value.imag()));
    const auto scaled = value / scale;
    if ((value.real() != 0. && scaled.real() == 0.) ||
        (value.imag() != 0. && scaled.imag() == 0.)) {
        throw std::overflow_error("phase response loses a complex component");
    }
    int exponent{};
    const double mantissa = std::frexp(scale, &exponent);
    return {scaled, mantissa, exponent};
}

// No intermediate norm(a)*norm(H)*PSD, no log/exp round-trip and no reliance
// on a wider long double ABI. Bounded mantissas keep ordinary products stable.
inline std::pair<Complex, Complex> coefficients(Complex wave, Complex response, double psd) {
    if (psd == 0. || response == Complex{}) {
        return {};
    }
    const auto a = decompose(wave), h = decompose(response);
    int exponent{};
    double mantissa = std::frexp(psd, &exponent);
    --exponent; // S_phi / 2, without underflow of the input PSD.
    if (exponent % 2 != 0) {
        mantissa *= 2.;
        --exponent;
    }
    const double scale = a.mantissa * h.mantissa * std::sqrt(mantissa);
    exponent = a.exponent + h.exponent + exponent / 2;
    auto make = [&](Complex phase) {
        phase = {product(phase.real(), scale), product(phase.imag(), scale)};
        const Complex result{std::scalbn(phase.real(), exponent),
                             std::scalbn(phase.imag(), exponent)};
        if (!conversion_detail::finite(result) || (phase.real() != 0. && result.real() == 0.) ||
            (phase.imag() != 0. && result.imag() == 0.)) {
            throw std::overflow_error("phase response coefficient is not representable");
        }
        const auto density = std::norm(result);
        if (!std::isfinite(density) || density < std::numeric_limits<double>::min()) {
            throw std::overflow_error("phase response density must be finite and normal");
        }
        return result;
    };
    return {make(multiply(Complex{0., 1.} * a.phase, h.phase)),
            make(multiply(Complex{0., 1.} * a.phase, std::conj(h.phase)))};
}

inline void add(Complex &target, Complex value) {
    if (!conversion_detail::finite(value)) {
        throw std::overflow_error("phase response product overflow");
    }
    target += value;
    if (!conversion_detail::finite(target)) {
        throw std::overflow_error("phase response accumulation overflow");
    }
}
} // namespace phase_response_detail

// A group is an explicitly shared real reference phase process, not an inferred
// physical common clock. Different positive offsets are independent. With the
// exp(+j*2*pi*f*t) convention H(-f)=conj(H(+f)). Each offset uses one proper u:
// U_upper=j*a*H*sqrt(S_phi/2), V_lower=j*a*conj(H)*sqrt(S_phi/2).
// Sum coincident latent amplitudes BEFORE C=UU^H+VV^H and P=UV^T+VU^T.
// The returned complete C/P is a density in W/Hz; no bin-width factor is applied.
inline ConversionNoise phase_response_group(const std::vector<ConversionChannel> &channels,
                                            const std::vector<PhaseResponseCarrier> &carriers,
                                            double spacing_hz,
                                            const std::vector<PhaseResponsePoint> &points) {
    const auto n = channels.size();
    if (!n || n > 512 || carriers.empty() || carriers.size() > n || points.empty() ||
        points.size() > 255 || !std::isfinite(spacing_hz) || spacing_hz <= 0.) {
        throw std::invalid_argument("invalid phase response shape or spacing");
    }
    std::map<std::pair<std::size_t, int>, std::size_t> indices;
    for (std::size_t i = 0; i < n; ++i) {
        if (channels[i].port >= 1024 || channels[i].bin < 0 ||
            !indices.emplace(std::make_pair(channels[i].port, channels[i].bin), i).second) {
            throw std::invalid_argument("invalid or duplicate phase response channel");
        }
    }
    std::set<std::size_t> seen;
    for (const auto &carrier : carriers) {
        if (carrier.channel >= n || !seen.insert(carrier.channel).second ||
            channels[carrier.channel].bin <= 0 || !conversion_detail::finite(carrier.wave) ||
            carrier.wave == Complex{}) {
            throw std::invalid_argument("invalid phase response carrier");
        }
    }
    ConversionNoise result{conversion_detail::zero(n), conversion_detail::zero(n)};
    int previous = 0;
    for (const auto &point : points) {
        const double hz = point.offset_bin * spacing_hz;
        if (point.offset_bin <= previous || !std::isfinite(hz) || hz <= 0. ||
            !std::isfinite(point.offset_hz) || point.offset_hz != hz ||
            !std::isfinite(point.reference_phase_rad2_per_hz) ||
            point.reference_phase_rad2_per_hz < 0. ||
            point.member_response.size() != carriers.size()) {
            throw std::invalid_argument("invalid phase response point, PSD or response count");
        }
        previous = point.offset_bin;
        std::vector<Complex> upper(n), lower(n);
        for (std::size_t k = 0; k < carriers.size(); ++k) {
            const auto &member = carriers[k];
            const auto channel = channels[member.channel];
            if (!conversion_detail::finite(point.member_response[k]) ||
                point.offset_bin >= channel.bin ||
                point.offset_bin > std::numeric_limits<int>::max() - channel.bin) {
                throw std::invalid_argument("invalid phase response or sidebands at/below DC");
            }
            const auto l = indices.find({channel.port, channel.bin - point.offset_bin});
            const auto u = indices.find({channel.port, channel.bin + point.offset_bin});
            if (l == indices.end() || u == indices.end()) {
                throw std::invalid_argument("both phase response sidebands must be declared");
            }
            const auto factor = phase_response_detail::coefficients(
                member.wave, point.member_response[k], point.reference_phase_rad2_per_hz);
            phase_response_detail::add(upper[u->second], factor.first);
            phase_response_detail::add(lower[l->second], factor.second);
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                phase_response_detail::add(
                    result.covariance(i, j),
                    phase_response_detail::multiply(upper[i], std::conj(upper[j])));
                phase_response_detail::add(
                    result.covariance(i, j),
                    phase_response_detail::multiply(lower[i], std::conj(lower[j])));
                phase_response_detail::add(result.complementary(i, j),
                                           phase_response_detail::multiply(upper[i], lower[j]));
                phase_response_detail::add(result.complementary(i, j),
                                           phase_response_detail::multiply(lower[i], upper[j]));
            }
        }
    }
    return result;
}
} // namespace rfmodel
