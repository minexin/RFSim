#pragma once
#include "rfmodel/conversion/frequency_conversion.hpp"

namespace rfmodel {
namespace conversion_power_wave_detail {
inline SMatrix quadrature(const std::vector<ConversionChannel> &channels,
                          const SMatrix &direct,
                          const SMatrix &conjugate) {
    const auto n = channels.size();
    auto result = conversion_detail::zero(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const auto sum = direct(i, j) + conjugate(i, j);
            const auto difference = direct(i, j) - conjugate(i, j);
            result(2 * i, 2 * j) = sum.real();
            result(2 * i + 1, 2 * j) = sum.imag();
            if (channels[j].bin != 0) {
                result(2 * i, 2 * j + 1) = -difference.imag();
                result(2 * i + 1, 2 * j + 1) = difference.real();
            }
        }
    }
    noise_detail::finite_matrix(result);
    return result;
}

inline std::pair<SMatrix, SMatrix> complex_linear(const SMatrix &matrix,
                                                  const std::vector<ConversionChannel> &channels) {
    const auto n = channels.size();
    auto direct = conversion_detail::zero(n), conjugate = direct;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            // DC has one real degree of freedom. Discard only the unused
            // imaginary input/output coordinates, never a physical RF component.
            const double xx = matrix(2 * i, 2 * j).real();
            const double xy = channels[j].bin ? matrix(2 * i, 2 * j + 1).real() : 0.;
            const double yx = channels[i].bin ? matrix(2 * i + 1, 2 * j).real() : 0.;
            const double yy =
                channels[i].bin && channels[j].bin ? matrix(2 * i + 1, 2 * j + 1).real() : 0.;
            direct(i, j) = {xx * .5 + yy * .5, yx * .5 - xy * .5};
            conjugate(i, j) = {xx * .5 - yy * .5, yx * .5 + xy * .5};
        }
    }
    noise_detail::finite_matrix(direct);
    noise_detail::finite_matrix(conjugate);
    return {std::move(direct), std::move(conjugate)};
}

inline void new_references(const FrequencyConversionModel &model,
                           const std::vector<Complex> &references) {
    power_wave_detail::references(references, model.channels().size());
    for (std::size_t i = 0; i < references.size(); ++i) {
        if (model.channels()[i].bin == 0 && references[i].imag() != 0.) {
            throw std::invalid_argument("DC conversion reference must be real");
        }
    }
}
} // namespace conversion_power_wave_detail

struct ConversionPowerWaveRenormalization {
    FrequencyConversionModel model;
    // An old outgoing affine offset/noise c becomes U*c + V*conj(c).
    SMatrix noise_direct, noise_conjugate;
};

// Change coordinates of the complete widely-linear relation, including B.
// Source/load boundary coordinates are separate; use the boundary helper below.
inline ConversionPowerWaveRenormalization
renormalize_power_waves(const FrequencyConversionModel &model,
                        const std::vector<Complex> &new_references) {
    conversion_power_wave_detail::new_references(model, new_references);
    const auto &channels = model.channels();
    const auto &old_references = model.reference_impedances_ohms();
    const auto n = channels.size(), m = 2 * n;
    if (new_references == old_references) {
        auto identity = conversion_detail::zero(n);
        for (std::size_t i = 0; i < n; ++i) {
            identity(i, i) = 1.;
        }
        return {model, identity, conversion_detail::zero(n)};
    }
    const auto original =
        conversion_power_wave_detail::quadrature(channels, model.direct(), model.conjugate());
    auto u = conversion_detail::zero(m), v = u, w = u, x = u;
    const auto assign = [](SMatrix &matrix, std::size_t i, Complex value) {
        matrix(2 * i, 2 * i) = value.real();
        matrix(2 * i, 2 * i + 1) = -value.imag();
        matrix(2 * i + 1, 2 * i) = value.imag();
        matrix(2 * i + 1, 2 * i + 1) = value.real();
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto coordinates =
            power_wave_detail::coordinates(old_references[i], new_references[i]);
        assign(u, i, coordinates.incident_from_incident);
        assign(v, i, coordinates.incident_from_outgoing);
        assign(w, i, coordinates.outgoing_from_incident);
        assign(x, i, coordinates.outgoing_from_outgoing);
    }
    // Inverse coordinates: a=X*a'-V*b', b=-W*a'+U*b'.
    // Solve (U+M*V)*b'=(W+M*X)*a'+c. Its inverse is the noise
    // transfer directly; subtracting X-M'*V loses small finite coefficients.
    auto equation = conversion_detail::multiply(original, v);
    auto right = conversion_detail::multiply(original, x);
    auto identity = conversion_detail::zero(m);
    for (std::size_t i = 0; i < equation.values.size(); ++i) {
        equation.values[i] += u.values[i];
        right.values[i] += w.values[i];
    }
    for (std::size_t i = 0; i < m; ++i) {
        identity(i, i) = 1.;
    }
    noise_detail::finite_matrix(equation);
    noise_detail::finite_matrix(right);
    const auto changed = parameter_detail::solve(equation, right, 1.);
    const auto noise = parameter_detail::solve(equation, identity, 1.);
    const auto coefficients = conversion_power_wave_detail::complex_linear(changed, channels);
    const auto transfer = conversion_power_wave_detail::complex_linear(noise, channels);
    return {
        FrequencyConversionModel(
            model.spacing_hz(), channels, coefficients.first, coefficients.second, new_references),
        transfer.first,
        transfer.second};
}

inline std::vector<Complex>
renormalize_conversion_offset(const ConversionPowerWaveRenormalization &change,
                              const std::vector<Complex> &offset) {
    const auto n = change.model.channels().size();
    if (offset.size() != n) {
        throw std::invalid_argument("conversion offset dimensions differ");
    }
    noise_detail::finite_matrix(change.noise_direct);
    noise_detail::finite_matrix(change.noise_conjugate);
    if (change.noise_direct.ports != n || change.noise_conjugate.ports != n) {
        throw std::invalid_argument("conversion noise transfer dimensions differ");
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!conversion_detail::finite(offset[i]) ||
            (change.model.channels()[i].bin == 0 && offset[i].imag() != 0.)) {
            throw std::invalid_argument("invalid conversion offset or complex DC");
        }
    }
    std::vector<Complex> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            result[i] += change.noise_direct(i, j) * offset[j] +
                         change.noise_conjugate(i, j) * std::conj(offset[j]);
        }
        if (!conversion_detail::finite(result[i]) ||
            (change.model.channels()[i].bin == 0 && result[i].imag() != 0.)) {
            throw std::overflow_error("conversion transformed offset is invalid");
        }
    }
    return result;
}

inline ConversionNoise
renormalize_conversion_noise(const ConversionPowerWaveRenormalization &change,
                             const ConversionNoise &noise) {
    change.model.validate_noise(noise);
    // Reuse the conversion kernel to validate the complete U/V transfer and
    // propagate C and P jointly, with no boundary reflection or extra noise.
    const FrequencyConversionModel transfer(change.model.spacing_hz(),
                                            change.model.channels(),
                                            change.noise_direct,
                                            change.noise_conjugate,
                                            change.model.reference_impedances_ohms());
    const auto zero = transfer.zero_noise();
    const std::vector<Complex> sources(transfer.channels().size());
    return transfer.analyze(sources, sources, noise, zero).outgoing_noise;
}

struct ConversionPowerWaveBoundary {
    std::vector<Complex> source, reflection;
    ConversionNoise source_noise;
};

// Preserve the physical sources/loads at explicitly selected external channels.
// Other channels must have zero boundaries/noise and remain available for wires.
inline ConversionPowerWaveBoundary
renormalize_conversion_boundary(const FrequencyConversionModel &model,
                                const std::vector<Complex> &new_references,
                                const std::vector<std::size_t> &external_channels,
                                const std::vector<Complex> &source,
                                const std::vector<Complex> &reflection,
                                const ConversionNoise &source_noise) {
    conversion_power_wave_detail::new_references(model, new_references);
    const auto n = model.channels().size();
    if (source.size() != n || reflection.size() != n || external_channels.size() > n) {
        throw std::invalid_argument("conversion boundary dimensions differ");
    }
    model.validate_noise(source_noise);
    std::vector<bool> selected(n);
    for (const auto i : external_channels) {
        if (i >= n || selected[i]) {
            throw std::invalid_argument("invalid or duplicate external conversion channel");
        }
        selected[i] = true;
    }
    ConversionPowerWaveBoundary result{
        std::vector<Complex>(n), std::vector<Complex>(n), model.zero_noise()};
    std::vector<Complex> transfer(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!conversion_detail::finite(source[i]) || !conversion_detail::finite(reflection[i]) ||
            (model.channels()[i].bin == 0 &&
             (source[i].imag() != 0. || reflection[i].imag() != 0.))) {
            throw std::invalid_argument("invalid conversion boundary or complex DC");
        }
        if (!selected[i]) {
            if (source[i] != Complex{} || reflection[i] != Complex{}) {
                throw std::invalid_argument("unselected conversion channel has a boundary");
            }
            for (std::size_t j = 0; j < n; ++j) {
                if (source_noise.covariance(i, j) != Complex{} ||
                    source_noise.covariance(j, i) != Complex{} ||
                    source_noise.complementary(i, j) != Complex{} ||
                    source_noise.complementary(j, i) != Complex{}) {
                    throw std::invalid_argument("unselected conversion channel has boundary noise");
                }
            }
            continue;
        }
        const auto coordinates =
            power_wave_detail::coordinates(model.reference_impedances_ohms()[i], new_references[i]);
        const auto reflected = coordinates.outgoing_from_incident * reflection[i];
        const auto outgoing = coordinates.outgoing_from_outgoing;
        const auto denominator = reflected + outgoing;
        if (!conversion_detail::finite(denominator)) {
            throw std::overflow_error("conversion boundary reference denominator overflow");
        }
        // Reject cancellation relative to the two terms, not an absolute floor:
        // exact physical poles can leave a nonzero rounding residual. Scale
        // components before taking norms so this check cannot overflow.
        const double term_scale = std::max({std::abs(reflected.real()),
                                            std::abs(reflected.imag()),
                                            std::abs(outgoing.real()),
                                            std::abs(outgoing.imag())});
        if (term_scale == 0. ||
            std::abs(denominator / term_scale) <=
                64. * std::numeric_limits<double>::epsilon() *
                    (std::abs(reflected / term_scale) + std::abs(outgoing / term_scale))) {
            throw std::domain_error("singular conversion boundary reference change");
        }
        result.reflection[i] = (coordinates.incident_from_incident * reflection[i] +
                                coordinates.incident_from_outgoing) /
                               denominator;
        // The coordinate determinant is one. Use the reciprocal instead of
        // subtracting two large terms in u-Gamma'*w. Normalize before inversion.
        const double scale = std::max(std::abs(denominator.real()), std::abs(denominator.imag()));
        const auto scaled = denominator / scale;
        transfer[i] = (std::conj(scaled) / std::norm(scaled)) / scale;
        result.source[i] = transfer[i] * source[i];
        if (!conversion_detail::finite(transfer[i]) ||
            !conversion_detail::finite(result.source[i]) ||
            !conversion_detail::finite(result.reflection[i])) {
            throw std::overflow_error("conversion boundary reference change overflow");
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            result.source_noise.covariance(i, j) =
                transfer[i] * source_noise.covariance(i, j) * std::conj(transfer[j]);
            result.source_noise.complementary(i, j) =
                transfer[i] * source_noise.complementary(i, j) * transfer[j];
        }
    }
    model.validate_noise(result.source_noise);
    return result;
}
} // namespace rfmodel
