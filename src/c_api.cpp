#include "rfmodel/monte_carlo.hpp"
#include "rfmodel/optimization.hpp"
#include <memory>
#include "rfmodel/conversion_frequency_plan.hpp"
#include "rfmodel/polynomial_linearization.hpp"
#include "rfmodel/amplifier_linearization.hpp"
#include "rfmodel/conversion_operating_point.hpp"
#include "rfmodel/channel_noise.hpp"
#include "rfmodel/phase_noise.hpp"
#include "rfmodel/mixer_linearization.hpp"
#include "rfmodel/conversion_network.hpp"
#include "rfmodel/conversion_power_wave.hpp"
#include "rfmodel/frequency_conversion.hpp"
#include "rfmodel/chebyshev_filter.hpp"
#include "rfmodel/butterworth_filter.hpp"
#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/rlc_model.hpp"
#include "rfmodel/multiport_devices.hpp"
#include "rfmodel/power_wave_reference.hpp"
#include "rfmodel/power_wave_noise.hpp"
#include "rfmodel/intermod_levels.hpp"
#include "rfmodel/coherent_highorder_amplifier.hpp"
#include "rfmodel/polynomial_intercepts.hpp"
#include "rfmodel/origin_expression.hpp"
#include "rfmodel/mixing_origin.hpp"
#include "rfmodel/coherent_polynomial.hpp"
#include "rfmodel/coherent_amplifier.hpp"
#include "rfmodel/coherent_mixer.hpp"
#include "rfmodel/source_coherence.hpp"
#include "rfmodel/coherent_network.hpp"
#include "rfmodel/coherence.hpp"
#include "rfmodel/c_api.h"
#include "rfmodel/fundamental_compression.hpp"
#include "rfmodel/saturating_fundamental.hpp"
#include "rfmodel/coherent_compression.hpp"
#include "rfmodel/single_tone_amplifier.hpp"
#include "rfmodel/multitone_amplifier.hpp"
#include "rfmodel/term_propagation.hpp"
#include "rfmodel/network.hpp"
#include "rfmodel/power_wave_network.hpp"
#include "rfmodel/loaded_noise.hpp"
#include "rfmodel/amplifier_model.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
#include "rfmodel/highorder_linearization.hpp"
#include "rfmodel/am_pm_linearization.hpp"
#include "rfmodel/ideal_mixer.hpp"
#include "rfmodel/spectrum_analysis.hpp"
#include "rfmodel/tabulated_model.hpp"
#include "rfmodel/tabulated_noise_model.hpp"
#include "rfmodel/noise_renormalization.hpp"
#include "rfmodel/network_parameters.hpp"
#include "rfmodel/transmission_line.hpp"
#include "rfmodel/rlgc_transmission_line.hpp"
#include <cstdio>
#include <new>

struct rfmodel_monte_carlo_result {
    rfmodel::MonteCarloResult core;
    size_t parameter_count{};
};

struct rfmodel_optimization_result {
    rfmodel::OptimizationResult core;
};

struct rfmodel_network {
    rfmodel::LinearNetwork core;
    size_t ports{};

    explicit rfmodel_network(double reference) : core(reference) {
    }
};

struct rfmodel_touchstone {
    size_t noise_samples;
    rfmodel::OutOfBand policy;
    rfmodel::TabulatedSParameterModel core;

    rfmodel_touchstone(rfmodel::TouchstoneData data, rfmodel::OutOfBand range_policy)
        : noise_samples(data.noise_samples.size()), policy(range_policy),
          core("Touchstone C API", std::move(data), range_policy) {
    }
};

namespace {
thread_local char last_error[512]{};

template <class Action> int guarded(Action action) noexcept {
    last_error[0] = '\0';
    try {
        action();
        return RFMODEL_OK;
    } catch (const std::bad_alloc &) {
        std::snprintf(last_error, sizeof(last_error), "allocation failed");
        return RFMODEL_OUT_OF_MEMORY;
    } catch (const std::invalid_argument &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_INVALID_ARGUMENT;
    } catch (const std::out_of_range &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_INVALID_ARGUMENT;
    } catch (const std::domain_error &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_INVALID_ARGUMENT;
    } catch (const std::length_error &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_INVALID_ARGUMENT;
    } catch (const std::runtime_error &error) {
        std::snprintf(last_error, sizeof(last_error), "%s", error.what());
        return RFMODEL_SOLVER_ERROR;
    } catch (...) {
        std::snprintf(last_error, sizeof(last_error), "unexpected internal error");
        return RFMODEL_INTERNAL_ERROR;
    }
}

void require(bool condition) {
    if (!condition) {
        throw std::invalid_argument("null pointer, invalid count or insufficient buffer");
    }
}

void disjoint(const void *a, size_t a_bytes, const void *b, size_t b_bytes) {
    if (a_bytes == 0 || b_bytes == 0) {
        return;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(a);
    const auto second = reinterpret_cast<std::uintptr_t>(b);
    require(a_bytes <= std::numeric_limits<std::uintptr_t>::max() - first);
    require(b_bytes <= std::numeric_limits<std::uintptr_t>::max() - second);
    require(first + a_bytes <= second || second + b_bytes <= first);
}

rfmodel::PowerWaveSpectrum
read_spectrum(double spacing, const rfmodel_spectrum_bin *input, size_t count) {
    require(count <= 2048 && (input || count == 0));
    rfmodel::PowerWaveSpectrum spectrum{spacing, {}};
    for (size_t i = 0; i < count; ++i) {
        const auto inserted = spectrum.amplitudes.emplace(
            input[i].index, rfmodel::Complex{input[i].amplitude.real, input[i].amplitude.imag});
        require(inserted.second);
    }
    rfmodel::validate_power_wave_spectrum(spectrum);
    return spectrum;
}

void write_spectrum(const rfmodel::PowerWaveSpectrum &spectrum,
                    rfmodel_spectrum_bin *output,
                    size_t capacity,
                    size_t *count) {
    require(count && capacity >= spectrum.amplitudes.size());
    require(output || spectrum.amplitudes.empty());
    size_t index = 0;
    for (const auto &entry : spectrum.amplitudes) {
        output[index++] = {entry.first, {entry.second.real(), entry.second.imag()}};
    }
    *count = index;
}

void write_coherence(const rfmodel::CoherentReduction &result,
                     rfmodel_coherent_component *groups,
                     size_t group_capacity,
                     size_t *group_count,
                     rfmodel_bin_power *powers,
                     size_t power_capacity,
                     size_t *power_count,
                     double *total_power_w) {
    require(group_count && power_count && total_power_w);
    require(group_capacity >= result.components.size() &&
            power_capacity >= result.power_by_bin_w.size());
    require((groups || result.components.empty()) && (powers || result.power_by_bin_w.empty()));
    for (size_t i = 0; i < result.components.size(); ++i) {
        const auto &value = result.components[i];
        groups[i] = {value.bin,
                     static_cast<int>(value.kind),
                     value.bandwidth_hz,
                     value.coherence_group,
                     {value.amplitude.real(), value.amplitude.imag()}};
    }
    size_t index = 0;
    for (const auto &entry : result.power_by_bin_w) {
        powers[index++] = {entry.first, entry.second};
    }
    *group_count = result.components.size();
    *power_count = result.power_by_bin_w.size();
    *total_power_w = result.total_power_w;
}

} // namespace

namespace {
static std::vector<rfmodel::OriginExpression>
decode_origin_expressions(const rfmodel_origin_expression *parents, size_t parent_count) {
    require(parent_count <= 4096 && (parents || parent_count == 0));
    std::vector<rfmodel::OriginExpression> result;
    size_t total_terms = 0, total_factors = 0;
    for (size_t i = 0; i < parent_count; ++i) {
        const auto &parent = parents[i];
        require(parent.term_count <= 4096 && parent.term_count <= 65536 - total_terms &&
                (parent.terms || parent.term_count == 0));
        total_terms += parent.term_count;
        rfmodel::OriginExpression expression;
        for (size_t j = 0; j < parent.term_count; ++j) {
            const auto &term = parent.terms[j];
            require(term.factors && term.factor_count >= 1 && term.factor_count <= 256);
            total_factors += term.factor_count;
            require(total_factors <= 1048576);
            rfmodel::MixingOrigin origin;
            for (size_t k = 0; k < term.factor_count; ++k) {
                origin.push_back({term.factors[k].root_id, term.factors[k].sign});
            }
            expression.push_back({std::move(origin), {term.amplitude.real, term.amplitude.imag}});
        }
        result.push_back(std::move(expression));
    }
    return result;
}

static void encode_origin_expression(const rfmodel::OriginExpression &result,
                                     rfmodel_origin_expression_term *terms,
                                     size_t term_capacity,
                                     size_t *term_count,
                                     rfmodel_origin_factor *factors,
                                     size_t factor_capacity,
                                     size_t *factor_count,
                                     rfmodel_complex *total_amplitude) {
    require(term_count && factor_count && total_amplitude);
    size_t required = 0;
    for (const auto &term : result) {
        required += term.factors.size();
    }
    require(term_capacity >= result.size() && factor_capacity >= required);
    require((terms || result.empty()) && (factors || required == 0));
    const auto total = rfmodel::origin_expression_amplitude(result);
    size_t offset = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        const auto &term = result[i];
        terms[i] = {offset, term.factors.size(), {term.amplitude.real(), term.amplitude.imag()}};
        for (const auto &factor : term.factors) {
            factors[offset++] = {factor.root_id, factor.sign};
        }
    }
    *term_count = result.size();
    *factor_count = required;
    *total_amplitude = {total.real(), total.imag()};
}

} // namespace

namespace {
template <std::size_t MaxOrder, class EncodedTerm>
int coherent_polynomial_evaluate_impl(double spacing_hz,
                                      const rfmodel_coherent_component *input,
                                      size_t input_count,
                                      const double *voltage_coefficients,
                                      size_t coefficient_count,
                                      double reference_ohms,
                                      uint64_t reserved_group_max,
                                      rfmodel_coherent_component *reduced_inputs,
                                      size_t reduced_capacity,
                                      size_t *reduced_count,
                                      EncodedTerm *terms,
                                      size_t term_capacity,
                                      size_t *term_count) {
    return guarded([&] {
        require(input_count <= 4096 && (input || input_count == 0));
        require(voltage_coefficients && coefficient_count >= 1 &&
                coefficient_count <= MaxOrder + 1);
        require(reduced_count && term_count);
        std::vector<rfmodel::CoherentComponent> components;
        for (size_t i = 0; i < input_count; ++i) {
            const auto &c = input[i];
            components.push_back({c.index,
                                  static_cast<rfmodel::SpectrumKind>(c.kind),
                                  c.bandwidth_hz,
                                  c.coherence_group,
                                  {c.amplitude.real, c.amplitude.imag}});
        }
        const rfmodel::CoherentPolynomial model(
            std::vector<double>(voltage_coefficients, voltage_coefficients + coefficient_count),
            reference_ohms);
        const auto result = model.evaluate(spacing_hz, components, reserved_group_max);
        require(reduced_capacity >= result.inputs.size() && term_capacity >= result.terms.size());
        require((reduced_inputs || result.inputs.empty()) && (terms || result.terms.empty()));
        auto encode = [](const rfmodel::CoherentComponent &c) {
            return rfmodel_coherent_component{c.bin,
                                              static_cast<int>(c.kind),
                                              c.bandwidth_hz,
                                              c.coherence_group,
                                              {c.amplitude.real(), c.amplitude.imag()}};
        };
        for (size_t i = 0; i < result.inputs.size(); ++i) {
            reduced_inputs[i] = encode(result.inputs[i]);
        }
        for (size_t i = 0; i < result.terms.size(); ++i) {
            const auto &term = result.terms[i];
            EncodedTerm encoded{};
            encoded.order = term.order;
            std::copy_n(term.input_indices.begin(), MaxOrder, encoded.input_indices);
            encoded.component = encode(term.component);
            terms[i] = encoded;
        }
        *reduced_count = result.inputs.size();
        *term_count = result.terms.size();
    });
}

} // namespace

namespace {
rfmodel::SMatrix decode_noise_two_port(const rfmodel_complex *values, size_t count) {
    require(values && count == 4);
    rfmodel::SMatrix matrix{2, std::vector<rfmodel::Complex>(4)};
    for (size_t i = 0; i < 4; ++i) {
        matrix.values[i] = {values[i].real, values[i].imag};
    }
    return matrix;
}

std::vector<rfmodel::Complex> decode_noise_references(const rfmodel_complex *references) {
    require(references);
    return {{references[0].real, references[0].imag}, {references[1].real, references[1].imag}};
}

void noise_output_disjoint(const rfmodel_complex *scattering,
                           const rfmodel_complex *noise,
                           const rfmodel_complex *references,
                           const void *output,
                           size_t bytes) {
    require(output);
    disjoint(scattering, 4 * sizeof(*scattering), output, bytes);
    disjoint(references, 2 * sizeof(*references), output, bytes);
    if (noise) {
        disjoint(noise, 4 * sizeof(*noise), output, bytes);
    }
}
} // namespace

namespace {
void write_passive_s(const rfmodel::SMatrix &matrix, rfmodel_complex *output, size_t capacity) {
    require(output && capacity >= matrix.values.size());
    for (size_t i = 0; i < matrix.values.size(); ++i) {
        output[i] = {matrix.values[i].real(), matrix.values[i].imag()};
    }
}
} // namespace

namespace {
std::vector<std::pair<const void *, size_t>>
conversion_input_ranges(const rfmodel_conversion_request *devices,
                        size_t device_count,
                        const rfmodel_conversion_connection *connections,
                        size_t connection_count,
                        size_t &total) {
    require(devices && device_count > 0 && device_count <= 512 && connection_count <= 512 &&
            (!connection_count || connections));
    total = 0;
    std::vector<std::pair<const void *, size_t>> inputs{
        {devices, device_count * sizeof(*devices)},
        {connections, connection_count * sizeof(*connections)}};
    for (size_t device = 0; device < device_count; ++device) {
        const auto &r = devices[device];
        const auto n = r.count;
        require(n > 0 && n <= 512 - total && r.physical_ports && r.bins && r.direct && r.conjugate);
        total += n;
        inputs.push_back({r.physical_ports, n * sizeof(size_t)});
        inputs.push_back({r.bins, n * sizeof(int)});
        for (const auto *matrix : {r.direct,
                                   r.conjugate,
                                   r.source_covariance,
                                   r.source_complementary,
                                   r.intrinsic_covariance,
                                   r.intrinsic_complementary}) {
            if (matrix) {
                inputs.push_back({matrix, n * n * sizeof(rfmodel_complex)});
            }
        }
        for (const auto *vector : {r.source, r.reflection}) {
            if (vector) {
                inputs.push_back({vector, n * sizeof(rfmodel_complex)});
            }
        }
    }
    return inputs;
}

void conversion_reference_ranges(const rfmodel_conversion_request *devices,
                                 size_t device_count,
                                 const rfmodel_conversion_references *references,
                                 std::vector<std::pair<const void *, size_t>> &inputs) {
    require(references);
    inputs.push_back({references, device_count * sizeof(*references)});
    for (size_t i = 0; i < device_count; ++i) {
        require(references[i].count == devices[i].count && references[i].values);
        inputs.push_back({references[i].values, references[i].count * sizeof(rfmodel_complex)});
    }
}

std::vector<rfmodel::Complex>
decode_conversion_references(const rfmodel_conversion_references &references) {
    std::vector<rfmodel::Complex> values;
    values.reserve(references.count);
    for (size_t i = 0; i < references.count; ++i) {
        values.emplace_back(references.values[i].real, references.values[i].imag);
    }
    return values;
}

std::vector<rfmodel::ConversionDevice>
decode_conversion_devices(const rfmodel_conversion_request *devices,
                          size_t device_count,
                          const rfmodel_conversion_references *references = nullptr) {
    std::vector<rfmodel::ConversionDevice> decoded;
    for (size_t device = 0; device < device_count; ++device) {
        const auto &r = devices[device];
        const auto n = r.count;
        auto matrix = [&](const rfmodel_complex *values) {
            auto decoded = rfmodel::conversion_detail::zero(n);
            if (values) {
                for (size_t i = 0; i < n * n; ++i) {
                    decoded.values[i] = {values[i].real, values[i].imag};
                }
            }
            return decoded;
        };
        std::vector<rfmodel::ConversionChannel> channels;
        std::vector<rfmodel::Complex> source(n), reflection(n);
        for (size_t i = 0; i < n; ++i) {
            channels.push_back({r.physical_ports[i], r.bins[i]});
            if (r.source) {
                source[i] = {r.source[i].real, r.source[i].imag};
            }
            if (r.reflection) {
                reflection[i] = {r.reflection[i].real, r.reflection[i].imag};
            }
        }
        rfmodel::FrequencyConversionModel model =
            references ? rfmodel::FrequencyConversionModel(
                             r.spacing_hz,
                             channels,
                             matrix(r.direct),
                             matrix(r.conjugate),
                             decode_conversion_references(references[device]))
                       : rfmodel::FrequencyConversionModel(r.spacing_hz,
                                                           channels,
                                                           matrix(r.direct),
                                                           matrix(r.conjugate),
                                                           r.reference_ohms);
        decoded.push_back({std::move(model),
                           source,
                           reflection,
                           {matrix(r.source_covariance), matrix(r.source_complementary)},
                           {matrix(r.intrinsic_covariance), matrix(r.intrinsic_complementary)}});
    }
    return decoded;
}

rfmodel::FrequencyConversionNetwork
decode_conversion_network(const rfmodel_conversion_request *devices,
                          size_t device_count,
                          const rfmodel_conversion_connection *connections,
                          size_t connection_count,
                          const rfmodel_conversion_references *references = nullptr) {
    rfmodel::FrequencyConversionNetwork network(devices[0].spacing_hz,
                                                references ? 50. : devices[0].reference_ohms);
    for (auto device : decode_conversion_devices(devices, device_count, references)) {
        if (references) {
            network.add_power_waves(std::move(device));
        } else {
            network.add(std::move(device));
        }
    }
    for (size_t i = 0; i < connection_count; ++i) {
        const auto &c = connections[i];
        network.connect(c.first_device, c.first_port, c.second_device, c.second_port);
    }
    return network;
}
} // namespace

namespace {
rfmodel::ConversionNonlinearDevice nonlinear_mixer(size_t device,
                                                   const rfmodel::FrequencyConversionModel &base,
                                                   rfmodel_bilinear_mixer_parameters parameters) {
    const auto channels = base.channels();
    const double spacing = base.spacing_hz(), reference = base.reference_ohms();
    return {device, [=](const std::vector<rfmodel::Complex> &wave) {
                const auto point =
                    rfmodel::linearize_bilinear_mixer(spacing,
                                                      channels,
                                                      wave,
                                                      parameters.gain_db,
                                                      parameters.lo_reference_amplitude,
                                                      parameters.rf_port,
                                                      parameters.lo_port,
                                                      parameters.if_port,
                                                      reference);
                return rfmodel::ConversionLinearization{point.incremental_model,
                                                        point.operating_outgoing};
            }};
}

rfmodel::ConversionLinearization
amplifier_point(double spacing,
                const std::vector<rfmodel::ConversionChannel> &channels,
                const std::vector<rfmodel::Complex> &wave,
                const rfmodel_saturating_amplifier_parameters &parameters,
                double reference) {
    require(parameters.include_output_drive == 0 || parameters.include_output_drive == 1);
    const rfmodel::SaturatingFundamentalCompression amplifier(
        parameters.power_gain_db, parameters.output_p1db_dbm, parameters.output_saturation_dbm);
    std::vector<size_t> drive{parameters.input_port};
    if (parameters.include_output_drive) {
        drive.push_back(parameters.output_port);
    }
    return rfmodel::linearize_saturating_amplifier(spacing,
                                                   channels,
                                                   wave,
                                                   amplifier,
                                                   parameters.input_port,
                                                   parameters.output_port,
                                                   drive,
                                                   reference);
}

std::pair<const void *, size_t>
polynomial_coefficient_range(const rfmodel_polynomial_amplifier_parameters &parameters) {
    require(parameters.voltage_coefficients && parameters.coefficient_count > 0 &&
            parameters.coefficient_count <= rfmodel::maximum_polynomial_order + 1);
    return {parameters.voltage_coefficients, parameters.coefficient_count * sizeof(double)};
}

rfmodel::MemorylessPolynomial
polynomial_model(const rfmodel_polynomial_amplifier_parameters &parameters) {
    polynomial_coefficient_range(parameters);
    return rfmodel::MemorylessPolynomial(
        std::vector<double>(parameters.voltage_coefficients,
                            parameters.voltage_coefficients + parameters.coefficient_count));
}

std::vector<std::pair<const void *, size_t>>
highorder_ranges(const rfmodel_highorder_amplifier_parameters &p) {
    require(p.coefficient_count <= 10 &&
            (!p.coefficient_count || p.nonlinear_voltage_coefficients));
    require(p.generating_count <= 512 && (!p.generating_count || p.generating_bins));
    return {{p.nonlinear_voltage_coefficients, p.coefficient_count * sizeof(double)},
            {p.generating_bins, p.generating_count * sizeof(int)}};
}

rfmodel::ConversionLinearization
highorder_point(double spacing,
                const std::vector<rfmodel::ConversionChannel> &channels,
                const std::vector<rfmodel::Complex> &waves,
                const rfmodel_highorder_amplifier_parameters &p,
                double reference) {
    highorder_ranges(p);
    std::vector<double> coefficients;
    std::vector<int> generating;
    if (p.coefficient_count) {
        coefficients.assign(p.nonlinear_voltage_coefficients,
                            p.nonlinear_voltage_coefficients + p.coefficient_count);
    }
    if (p.generating_count) {
        generating.assign(p.generating_bins, p.generating_bins + p.generating_count);
    }
    return rfmodel::linearize_highorder_amplifier(
        spacing,
        channels,
        waves,
        rfmodel::CoherentHighOrderAmplifier(
            p.power_gain_db, p.output_p1db_dbm, p.output_saturation_dbm, coefficients, reference),
        generating,
        p.input_port,
        p.output_port);
}

std::pair<const void *, size_t> am_pm_range(const rfmodel_am_pm_parameters &p) {
    require(p.output_count <= 512 && (!p.output_count || p.output_channels));
    return {p.output_channels, p.output_count * sizeof(size_t)};
}

std::vector<size_t> am_pm_channels(const rfmodel_am_pm_parameters &p) {
    am_pm_range(p);
    if (!p.output_count) {
        return {};
    }
    return {p.output_channels, p.output_channels + p.output_count};
}

void analyze_conversion_network_outputs(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded,
    const rfmodel_conversion_source_noise *additional = nullptr,
    const rfmodel_conversion_affine_offset *offset = nullptr,
    const rfmodel_conversion_operating_options *operating = nullptr,
    const rfmodel_conversion_bilinear_mixer *mixers = nullptr,
    size_t mixer_count = 0,
    rfmodel_conversion_operating_diagnostics *diagnostics = nullptr,
    const rfmodel_conversion_nonlinear_model *models = nullptr,
    size_t model_count = 0,
    const rfmodel_conversion_am_pm *phases = nullptr,
    size_t phase_count = 0,
    const rfmodel_conversion_references *references = nullptr) {
    require(output && !(operating && references));
    const auto &o = *output;
    size_t total;
    auto inputs =
        conversion_input_ranges(devices, device_count, connections, connection_count, total);
    if (references) {
        conversion_reference_ranges(devices, device_count, references, inputs);
    }
    if (additional) {
        require(additional->count == total && additional->covariance);
        inputs.push_back({additional, sizeof(*additional)});
        inputs.push_back({additional->covariance, total * total * sizeof(rfmodel_complex)});
        if (additional->complementary) {
            inputs.push_back({additional->complementary, total * total * sizeof(rfmodel_complex)});
        }
    }
    if (offset) {
        require(offset->count == total && offset->values);
        inputs.push_back({offset, sizeof(*offset)});
        inputs.push_back({offset->values, total * sizeof(rfmodel_complex)});
    }
    inputs.push_back({output, sizeof(*output)});
    require(o.incident && o.outgoing && o.noise_covariance && o.noise_complementary &&
            o.relative_residual);
    require(o.wave_capacity >= total && o.matrix_capacity >= total * total &&
            o.wave_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex) &&
            o.matrix_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
    std::vector<std::pair<const void *, size_t>> outputs{
        {o.incident, o.wave_capacity * sizeof(rfmodel_complex)},
        {o.outgoing, o.wave_capacity * sizeof(rfmodel_complex)},
        {o.noise_covariance, o.matrix_capacity * sizeof(rfmodel_complex)},
        {o.noise_complementary, o.matrix_capacity * sizeof(rfmodel_complex)},
        {o.relative_residual, sizeof(double)}};
    if (loaded) {
        inputs.push_back({loaded, sizeof(*loaded)});
        require(loaded->incident_covariance && loaded->incident_complementary &&
                loaded->incident_outgoing_covariance && loaded->incident_outgoing_complementary &&
                loaded->net_noise_into_device_w_per_hz &&
                loaded->matrix_capacity >= total * total && loaded->power_capacity >= total &&
                loaded->matrix_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex) &&
                loaded->power_capacity <= std::numeric_limits<size_t>::max() / sizeof(double));
        for (auto *matrix : {loaded->incident_covariance,
                             loaded->incident_complementary,
                             loaded->incident_outgoing_covariance,
                             loaded->incident_outgoing_complementary}) {
            outputs.push_back({matrix, loaded->matrix_capacity * sizeof(rfmodel_complex)});
        }
        outputs.push_back(
            {loaded->net_noise_into_device_w_per_hz, loaded->power_capacity * sizeof(double)});
    }
    if (operating) {
        require(diagnostics && mixer_count <= device_count && (!mixer_count || mixers));
        require((!operating->initial_incident && operating->initial_count == 0) ||
                (operating->initial_incident && operating->initial_count == total));
        inputs.push_back({operating, sizeof(*operating)});
        inputs.push_back({mixers, mixer_count * sizeof(*mixers)});
        require(model_count <= device_count && mixer_count + model_count <= device_count &&
                (!model_count || models));
        inputs.push_back({models, model_count * sizeof(*models)});
        require(phase_count <= model_count && (!phase_count || phases));
        inputs.push_back({phases, phase_count * sizeof(*phases)});
        for (size_t i = 0; i < phase_count; ++i) {
            inputs.push_back(am_pm_range(phases[i].phase));
        }

        for (size_t i = 0; i < model_count; ++i) {
            require(models[i].parameters);
            size_t bytes = 0;
            switch (models[i].kind) {
            case RFMODEL_NONLINEAR_BILINEAR_MIXER:
                bytes = sizeof(rfmodel_bilinear_mixer_parameters);
                break;
            case RFMODEL_NONLINEAR_SATURATING_AMPLIFIER:
                bytes = sizeof(rfmodel_saturating_amplifier_parameters);
                break;
            case RFMODEL_NONLINEAR_POLYNOMIAL_AMPLIFIER:
                bytes = sizeof(rfmodel_polynomial_amplifier_parameters);
                inputs.push_back(polynomial_coefficient_range(
                    *static_cast<const rfmodel_polynomial_amplifier_parameters *>(
                        models[i].parameters)));
                break;
            case RFMODEL_NONLINEAR_HIGHORDER_AMPLIFIER:
                bytes = sizeof(rfmodel_highorder_amplifier_parameters);
                for (const auto &range :
                     highorder_ranges(*static_cast<const rfmodel_highorder_amplifier_parameters *>(
                         models[i].parameters))) {
                    inputs.push_back(range);
                }
                break;
            default:
                throw std::invalid_argument("unknown nonlinear conversion model kind");
            }
            inputs.push_back({models[i].parameters, bytes});
        }

        inputs.push_back(
            {operating->initial_incident, operating->initial_count * sizeof(rfmodel_complex)});
        outputs.push_back({diagnostics, sizeof(*diagnostics)});
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
        for (const auto &input : inputs) {
            if (input.second) {
                disjoint(outputs[i].first, outputs[i].second, input.first, input.second);
            }
        }
        for (size_t j = 0; j < i; ++j) {
            disjoint(outputs[i].first, outputs[i].second, outputs[j].first, outputs[j].second);
        }
    }
    const auto network =
        decode_conversion_network(devices, device_count, connections, connection_count, references);
    rfmodel::ConversionNoise extra;
    if (additional) {
        extra = {rfmodel::conversion_detail::zero(total), rfmodel::conversion_detail::zero(total)};
        for (size_t i = 0; i < total * total; ++i) {
            extra.covariance.values[i] = {additional->covariance[i].real,
                                          additional->covariance[i].imag};
            if (additional->complementary) {
                extra.complementary.values[i] = {additional->complementary[i].real,
                                                 additional->complementary[i].imag};
            }
        }
    }
    std::vector<rfmodel::Complex> emission;
    if (offset) {
        emission.reserve(total);
        for (size_t i = 0; i < total; ++i) {
            emission.emplace_back(offset->values[i].real, offset->values[i].imag);
        }
    }
    rfmodel::ConversionResult result;
    rfmodel::ConversionOperatingResult solved;
    if (operating) {
        const auto decoded = decode_conversion_devices(devices, device_count);
        std::vector<rfmodel::ConversionNonlinearDevice> nonlinear;
        for (size_t i = 0; i < mixer_count; ++i) {
            require(mixers[i].device < device_count);
            nonlinear.push_back(nonlinear_mixer(
                mixers[i].device, decoded[mixers[i].device].model, mixers[i].model));
        }
        for (size_t i = 0; i < model_count; ++i) {
            const auto &spec = models[i];
            require(spec.device < device_count);
            const auto &base = decoded[spec.device].model;
            if (spec.kind == RFMODEL_NONLINEAR_BILINEAR_MIXER) {
                nonlinear.push_back(nonlinear_mixer(
                    spec.device,
                    base,
                    *static_cast<const rfmodel_bilinear_mixer_parameters *>(spec.parameters)));
            } else if (spec.kind == RFMODEL_NONLINEAR_HIGHORDER_AMPLIFIER) {
                const auto parameters =
                    *static_cast<const rfmodel_highorder_amplifier_parameters *>(spec.parameters);
                const auto channels = base.channels();
                const double spacing = base.spacing_hz(), reference = base.reference_ohms();
                nonlinear.push_back({spec.device, [=](const std::vector<rfmodel::Complex> &wave) {
                                         return highorder_point(
                                             spacing, channels, wave, parameters, reference);
                                     }});
            } else if (spec.kind == RFMODEL_NONLINEAR_POLYNOMIAL_AMPLIFIER) {
                const auto parameters =
                    *static_cast<const rfmodel_polynomial_amplifier_parameters *>(spec.parameters);
                const auto polynomial = polynomial_model(parameters);
                const auto channels = base.channels();
                const double spacing = base.spacing_hz(), reference = base.reference_ohms();
                nonlinear.push_back({spec.device, [=](const std::vector<rfmodel::Complex> &wave) {
                                         return rfmodel::linearize_polynomial_amplifier(
                                             spacing,
                                             channels,
                                             wave,
                                             polynomial,
                                             parameters.input_port,
                                             parameters.output_port,
                                             reference);
                                     }});
            } else {
                const auto parameters =
                    *static_cast<const rfmodel_saturating_amplifier_parameters *>(spec.parameters);
                const auto channels = base.channels();
                const double spacing = base.spacing_hz(), reference = base.reference_ohms();
                nonlinear.push_back({spec.device, [=](const std::vector<rfmodel::Complex> &wave) {
                                         return amplifier_point(
                                             spacing, channels, wave, parameters, reference);
                                     }});
            }
        }
        std::set<size_t> phased_devices;
        for (size_t i = 0; i < phase_count; ++i) {
            const auto &spec = phases[i];
            require(phased_devices.insert(spec.device).second);
            auto found = std::find_if(nonlinear.begin(), nonlinear.end(), [&](const auto &entry) {
                return entry.device == spec.device;
            });
            require(found != nonlinear.end());
            const auto evaluate = found->evaluate;
            const rfmodel::LegacyPointDerivativeAmPm law(spec.phase.peak_degrees_per_db,
                                                         spec.phase.output_reference_dbm);
            const auto selected = am_pm_channels(spec.phase);
            found->evaluate = [=](const std::vector<rfmodel::Complex> &wave) {
                return rfmodel::apply_output_am_pm(evaluate(wave), law, selected);
            };
        }
        std::vector<rfmodel::ConversionPortConnection> wires;
        for (size_t i = 0; i < connection_count; ++i) {
            const auto &w = connections[i];
            wires.push_back({w.first_device, w.first_port, w.second_device, w.second_port});
        }
        std::vector<rfmodel::Complex> initial;
        for (size_t i = 0; i < operating->initial_count; ++i) {
            initial.emplace_back(operating->initial_incident[i].real,
                                 operating->initial_incident[i].imag);
        }
        const rfmodel::ConversionOperatingOptions options{operating->max_iterations,
                                                          operating->max_backtracks,
                                                          operating->relative_tolerance,
                                                          operating->absolute_tolerance};
        solved = rfmodel::solve_conversion_operating_point(decoded,
                                                           wires,
                                                           nonlinear,
                                                           initial,
                                                           options,
                                                           loaded != nullptr,
                                                           additional ? &extra : nullptr,
                                                           emission);
        result = std::move(solved.waves);
    } else {
        result = network.analyze(loaded != nullptr, additional ? &extra : nullptr, emission);
    }

    for (size_t i = 0; i < total; ++i) {
        o.incident[i] = {result.incident[i].real(), result.incident[i].imag()};
        o.outgoing[i] = {result.outgoing[i].real(), result.outgoing[i].imag()};
    }
    for (size_t i = 0; i < total * total; ++i) {
        const auto c = result.outgoing_noise.covariance.values[i],
                   p = result.outgoing_noise.complementary.values[i];
        o.noise_covariance[i] = {c.real(), c.imag()};
        o.noise_complementary[i] = {p.real(), p.imag()};
    }
    if (operating) {
        *diagnostics = {solved.iterations, solved.backtracks, solved.scaled_residual};
    }
    *o.relative_residual = result.relative_residual;
    if (loaded) {
        const std::pair<rfmodel_complex *, const rfmodel::SMatrix *> matrices[] = {
            {loaded->incident_covariance, &result.incident_noise.covariance},
            {loaded->incident_complementary, &result.incident_noise.complementary},
            {loaded->incident_outgoing_covariance, &result.incident_outgoing_noise.covariance},
            {loaded->incident_outgoing_complementary,
             &result.incident_outgoing_noise.complementary}};
        for (const auto &matrix : matrices) {
            for (size_t i = 0; i < total * total; ++i) {
                const auto value = matrix.second->values[i];
                matrix.first[i] = {value.real(), value.imag()};
            }
        }
        for (size_t i = 0; i < total; ++i) {
            loaded->net_noise_into_device_w_per_hz[i] = result.net_noise_into_device_w_per_hz[i];
        }
    }
}
} // namespace

namespace {
template <class Request, class Evaluate>
void linearize_conversion_outputs(
    const Request *request,
    const rfmodel_mixer_linearization_output *output,
    Evaluate evaluate,
    const std::vector<std::pair<const void *, size_t>> &extra_inputs = {}) {
    require(request && output);
    const auto &r = *request;
    const auto &o = *output;
    require(r.count && r.count <= 512 && r.physical_ports && r.bins && r.operating_incident &&
            o.direct && o.conjugate && o.operating_outgoing &&
            o.matrix_capacity >= r.count * r.count && o.wave_capacity >= r.count &&
            o.matrix_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex) &&
            o.wave_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
    std::vector<std::pair<const void *, size_t>> inputs{
        {request, sizeof(*request)},
        {output, sizeof(*output)},
        {r.physical_ports, r.count * sizeof(size_t)},
        {r.bins, r.count * sizeof(int)},
        {r.operating_incident, r.count * sizeof(rfmodel_complex)}};
    inputs.insert(inputs.end(), extra_inputs.begin(), extra_inputs.end());
    const std::vector<std::pair<const void *, size_t>> outputs{
        {o.direct, o.matrix_capacity * sizeof(rfmodel_complex)},
        {o.conjugate, o.matrix_capacity * sizeof(rfmodel_complex)},
        {o.operating_outgoing, o.wave_capacity * sizeof(rfmodel_complex)}};
    for (size_t i = 0; i < outputs.size(); ++i) {
        for (const auto &input : inputs) {
            disjoint(outputs[i].first, outputs[i].second, input.first, input.second);
        }
        for (size_t j = 0; j < i; ++j) {
            disjoint(outputs[i].first, outputs[i].second, outputs[j].first, outputs[j].second);
        }
    }
    std::vector<rfmodel::ConversionChannel> channels;
    std::vector<rfmodel::Complex> waves;
    for (size_t i = 0; i < r.count; ++i) {
        channels.push_back({r.physical_ports[i], r.bins[i]});
        waves.push_back({r.operating_incident[i].real, r.operating_incident[i].imag});
    }
    const auto result = evaluate(r, channels, waves);
    for (size_t i = 0; i < r.count * r.count; ++i) {
        const auto a = result.jacobian.direct().values[i];
        const auto b = result.jacobian.conjugate().values[i];
        o.direct[i] = {a.real(), a.imag()};
        o.conjugate[i] = {b.real(), b.imag()};
    }
    for (size_t i = 0; i < r.count; ++i) {
        const auto value = result.outgoing[i];
        o.operating_outgoing[i] = {value.real(), value.imag()};
    }
}
} // namespace

namespace {
template <class Result>
size_t optimization_buffer(const Result *result,
                           const void *pointer,
                           size_t capacity,
                           size_t required,
                           size_t element_size) {
    require(capacity >= required && capacity <= std::numeric_limits<size_t>::max() / element_size);
    require(pointer || capacity == 0);
    const auto bytes = capacity * element_size;
    disjoint(pointer, bytes, result, sizeof(*result));
    return bytes;
}

void optimization_text(const std::string &source, char *output) {
    std::copy(source.begin(), source.end(), output);
    output[source.size()] = '\0';
}
} // namespace

extern "C" {
int rfmodel_run_monte_carlo(const rfmodel_monte_carlo_parameter *parameters,
                            size_t count,
                            size_t metric_count,
                            const rfmodel_monte_carlo_options *options,
                            rfmodel_monte_carlo_evaluator evaluator,
                            rfmodel_monte_carlo_cancel cancelled,
                            void *user_data,
                            rfmodel_monte_carlo_result **out) {
    return rfmodel_run_monte_carlo_extended(
        parameters, count, metric_count, options, nullptr, evaluator, cancelled, user_data, out);
}

int rfmodel_run_monte_carlo_extended(const rfmodel_monte_carlo_parameter *parameters,
                                     size_t count,
                                     size_t metric_count,
                                     const rfmodel_monte_carlo_options *options,
                                     const rfmodel_monte_carlo_extension *extension,
                                     rfmodel_monte_carlo_evaluator evaluator,
                                     rfmodel_monte_carlo_cancel cancelled,
                                     void *user_data,
                                     rfmodel_monte_carlo_result **out) {
    return guarded([&] {
        require(parameters && count > 0 && count <= 64 && metric_count > 0 && metric_count <= 64 &&
                evaluator && out);
        disjoint(out, sizeof(*out), parameters, count * sizeof(*parameters));
        rfmodel::MonteCarloOptions opt;
        if (options) {
            disjoint(out, sizeof(*out), options, sizeof(*options));
            const auto &o = *options;
            require((o.reject_invalid == 0 || o.reject_invalid == 1) &&
                    (o.record_history == 0 || o.record_history == 1));
            require(o.quantile_count <= 256 && (o.quantile_levels || o.quantile_count == 0));
            require((o.criteria_count == 0 || o.criteria_count == metric_count) &&
                    (o.criteria || o.criteria_count == 0));
            disjoint(out, sizeof(*out), o.quantile_levels, o.quantile_count * sizeof(double));
            disjoint(out, sizeof(*out), o.criteria, o.criteria_count * sizeof(*o.criteria));
            opt.samples = o.samples;
            opt.seed = o.seed;
            opt.invalid_sample = o.reject_invalid ? rfmodel::MonteCarloFailurePolicy::Reject
                                                  : rfmodel::MonteCarloFailurePolicy::Stop;
            opt.record_history = o.record_history != 0;
            opt.quantile_levels.clear();
            for (size_t i = 0; i < o.quantile_count; ++i) {
                opt.quantile_levels.push_back(o.quantile_levels[i]);
            }
            for (size_t i = 0; i < o.criteria_count; ++i) {
                const auto &c = o.criteria[i];
                require((c.has_lower == 0 || c.has_lower == 1) &&
                        (c.has_upper == 0 || c.has_upper == 1));
                require((c.has_lower || c.lower == 0.) && (c.has_upper || c.upper == 0.));
                opt.criteria.push_back(
                    {c.has_lower ? std::optional<double>{c.lower} : std::nullopt,
                     c.has_upper ? std::optional<double>{c.upper} : std::nullopt});
            }
        }
        if (extension) {
            disjoint(out, sizeof(*out), extension, sizeof(*extension));
            const auto &e = *extension;
            require((e.joint_statistics == 0 || e.joint_statistics == 1) &&
                    (e.correlation_count == 0 || e.correlation_count == count * count) &&
                    (e.latent_correlation || e.correlation_count == 0));
            disjoint(out, sizeof(*out), e.latent_correlation, e.correlation_count * sizeof(double));
            opt.joint_statistics = e.joint_statistics != 0;
            for (size_t i = 0; i < e.correlation_count; ++i) {
                opt.latent_correlation.push_back(e.latent_correlation[i]);
            }
        }
        require(*out == nullptr);
        std::vector<rfmodel::MonteCarloParameter> native;
        for (size_t i = 0; i < count; ++i) {
            native.push_back(
                {static_cast<rfmodel::MonteCarloDistribution>(parameters[i].distribution),
                 parameters[i].first,
                 parameters[i].second});
        }
        auto owned = std::make_unique<rfmodel_monte_carlo_result>();
        owned->parameter_count = count;
        owned->core = rfmodel::run_monte_carlo(
            native,
            metric_count,
            [&](const std::vector<double> &point) {
                std::vector<double> values(metric_count, std::numeric_limits<double>::quiet_NaN());
                size_t value_count = 0;
                char error[513]{};
                const int status = evaluator(point.data(),
                                             point.size(),
                                             user_data,
                                             values.data(),
                                             values.size(),
                                             &value_count,
                                             error,
                                             sizeof(error));
                error[512] = '\0';
                if (status != 0) {
                    throw std::runtime_error(error[0] ? error
                                                      : "Monte Carlo evaluator callback failed");
                }
                if (value_count != metric_count) {
                    values.clear();
                }
                return values;
            },
            opt,
            [&] {
                if (!cancelled) {
                    return false;
                }
                const int status = cancelled(user_data);
                if (status < 0) {
                    throw std::runtime_error("Monte Carlo cancellation callback failed");
                }
                return status > 0;
            });
        last_error[0] = '\0';
        *out = owned.release();
    });
}

void rfmodel_monte_carlo_destroy(rfmodel_monte_carlo_result *result) {
    delete result;
}

int rfmodel_monte_carlo_get_dependence(const rfmodel_monte_carlo_result *result,
                                       rfmodel_monte_carlo_dependence *info,
                                       double *effective_correlation,
                                       size_t capacity) {
    return guarded([&] {
        require(result && info);
        disjoint(info, sizeof(*info), result, sizeof(*result));
        const auto &r = result->core;
        const rfmodel_monte_carlo_dependence meta{r.effective_latent_correlation.size(),
                                                  r.latent_rank,
                                                  r.effective_latent_correlation.empty()
                                                      ? RFMODEL_MC_INDEPENDENT_V1
                                                      : RFMODEL_MC_GAUSSIAN_COPULA_V1,
                                                  r.parameter_joint.dimension ? 1 : 0};
        if (!effective_correlation && capacity == 0) {
            *info = meta;
            return;
        }
        const auto bytes = optimization_buffer(result,
                                               effective_correlation,
                                               capacity,
                                               r.effective_latent_correlation.size(),
                                               sizeof(double));
        disjoint(info, sizeof(*info), effective_correlation, bytes);
        if (!r.effective_latent_correlation.empty()) {
            std::copy(r.effective_latent_correlation.begin(),
                      r.effective_latent_correlation.end(),
                      effective_correlation);
        }
        *info = meta;
    });
}

int rfmodel_monte_carlo_get_joint_statistics(const rfmodel_monte_carlo_result *result,
                                             int parameters,
                                             rfmodel_monte_carlo_joint_statistics *info,
                                             rfmodel_monte_carlo_joint_value *values,
                                             size_t capacity) {
    return guarded([&] {
        require(result && info && (parameters == 0 || parameters == 1));
        disjoint(info, sizeof(*info), result, sizeof(*result));
        const auto &j = parameters ? result->core.parameter_joint : result->core.metric_joint;
        const rfmodel_monte_carlo_joint_statistics meta{j.dimension, j.count};
        if (!values && capacity == 0) {
            *info = meta;
            return;
        }
        const auto bytes = optimization_buffer(
            result, values, capacity, j.sample_covariance.size(), sizeof(*values));
        disjoint(info, sizeof(*info), values, bytes);
        for (size_t k = 0; k < j.sample_covariance.size(); ++k) {
            values[k] = {j.sample_covariance[k] ? 1 : 0,
                         j.sample_correlation[k] ? 1 : 0,
                         j.sample_covariance[k].value_or(0.),
                         j.sample_correlation[k].value_or(0.)};
        }
        *info = meta;
    });
}

int rfmodel_monte_carlo_get_summary(const rfmodel_monte_carlo_result *result,
                                    rfmodel_monte_carlo_summary *summary) {
    return guarded([&] {
        require(result && summary);
        disjoint(summary, sizeof(*summary), result, sizeof(*result));
        const auto &r = result->core;
        *summary = {result->parameter_count,
                    r.statistics.size(),
                    r.quantile_levels.size(),
                    r.history.size(),
                    r.requested_samples,
                    r.samples_attempted,
                    r.evaluations,
                    r.accepted_samples,
                    r.rejected_samples,
                    r.passed_samples,
                    r.last_failed_parameters.size(),
                    r.last_error.size(),
                    r.seed,
                    r.random_words,
                    static_cast<int>(r.stop),
                    r.has_statistics ? 1 : 0,
                    r.criteria_enabled ? 1 : 0,
                    r.passed_fraction ? 1 : 0,
                    r.valid_pass_fraction ? 1 : 0,
                    r.passed_fraction.value_or(0.),
                    r.valid_pass_fraction.value_or(0.)};
    });
}

int rfmodel_monte_carlo_get_quantile_levels(const rfmodel_monte_carlo_result *result,
                                            double *levels,
                                            size_t capacity) {
    return guarded([&] {
        require(result != nullptr);
        optimization_buffer(
            result, levels, capacity, result->core.quantile_levels.size(), sizeof(double));
        if (!result->core.quantile_levels.empty()) {
            std::copy(
                result->core.quantile_levels.begin(), result->core.quantile_levels.end(), levels);
        }
    });
}

int rfmodel_monte_carlo_get_statistics(const rfmodel_monte_carlo_result *result,
                                       size_t metric_index,
                                       rfmodel_monte_carlo_statistics *info,
                                       double *quantiles,
                                       size_t capacity) {
    return guarded([&] {
        require(result && info && metric_index < result->core.statistics.size());
        disjoint(info, sizeof(*info), result, sizeof(*result));
        const auto &s = result->core.statistics[metric_index];
        const rfmodel_monte_carlo_statistics meta{s.count,
                                                  s.quantiles.size(),
                                                  s.mean,
                                                  s.minimum,
                                                  s.maximum,
                                                  s.sample_variance ? 1 : 0,
                                                  s.sample_standard_deviation ? 1 : 0,
                                                  s.standard_error ? 1 : 0,
                                                  s.sample_variance.value_or(0.),
                                                  s.sample_standard_deviation.value_or(0.),
                                                  s.standard_error.value_or(0.)};
        if (!quantiles && capacity == 0) {
            *info = meta;
            return;
        }
        const auto bytes =
            optimization_buffer(result, quantiles, capacity, s.quantiles.size(), sizeof(double));
        disjoint(info, sizeof(*info), quantiles, bytes);
        if (!s.quantiles.empty()) {
            std::copy(s.quantiles.begin(), s.quantiles.end(), quantiles);
        }
        *info = meta;
    });
}

int rfmodel_monte_carlo_get_last_failure(const rfmodel_monte_carlo_result *result,
                                         double *parameters,
                                         size_t capacity,
                                         char *error,
                                         size_t error_capacity) {
    return guarded([&] {
        require(result != nullptr);
        const auto &r = result->core;
        const auto bytes = optimization_buffer(
            result, parameters, capacity, r.last_failed_parameters.size(), sizeof(double));
        const auto error_bytes =
            optimization_buffer(result, error, error_capacity, r.last_error.size() + 1, 1);
        disjoint(parameters, bytes, error, error_bytes);
        if (!r.last_failed_parameters.empty()) {
            std::copy(r.last_failed_parameters.begin(), r.last_failed_parameters.end(), parameters);
        }
        optimization_text(r.last_error, error);
    });
}

int rfmodel_monte_carlo_get_sample(const rfmodel_monte_carlo_result *result,
                                   size_t history_index,
                                   rfmodel_monte_carlo_sample *info,
                                   double *parameters,
                                   size_t parameter_capacity,
                                   double *values,
                                   size_t value_capacity,
                                   char *error,
                                   size_t error_capacity) {
    return guarded([&] {
        require(result && info && history_index < result->core.history.size());
        disjoint(info, sizeof(*info), result, sizeof(*result));
        const auto &s = result->core.history[history_index];
        const rfmodel_monte_carlo_sample meta{
            s.index, s.values.size(), s.error.size(), static_cast<int>(s.status), s.passed ? 1 : 0};
        if (!parameters && !parameter_capacity && !values && !value_capacity && !error &&
            !error_capacity) {
            *info = meta;
            return;
        }
        const auto pbytes = optimization_buffer(
            result, parameters, parameter_capacity, s.parameters.size(), sizeof(double));
        const auto vbytes =
            optimization_buffer(result, values, value_capacity, s.values.size(), sizeof(double));
        const auto ebytes =
            optimization_buffer(result, error, error_capacity, s.error.size() + 1, 1);
        const std::pair<const void *, size_t> outputs[] = {
            {info, sizeof(*info)}, {parameters, pbytes}, {values, vbytes}, {error, ebytes}};
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < i; ++j) {
                disjoint(outputs[i].first, outputs[i].second, outputs[j].first, outputs[j].second);
            }
        }
        std::copy(s.parameters.begin(), s.parameters.end(), parameters);
        if (!s.values.empty()) {
            std::copy(s.values.begin(), s.values.end(), values);
        }
        optimization_text(s.error, error);
        *info = meta;
    });
}

int rfmodel_optimize_bounded(const rfmodel_optimization_parameter *parameters,
                             size_t count,
                             const rfmodel_optimization_options *options,
                             rfmodel_optimization_objective objective,
                             rfmodel_optimization_cancel cancelled,
                             void *user_data,
                             rfmodel_optimization_result **out) {
    return guarded([&] {
        require(parameters && count > 0 && count <= 64 && objective && out);
        disjoint(out, sizeof(*out), parameters, count * sizeof(*parameters));
        if (options) {
            disjoint(out, sizeof(*out), options, sizeof(*options));
        }
        require(*out == nullptr);
        std::vector<rfmodel::OptimizationParameter> native;
        for (size_t i = 0; i < count; ++i) {
            const auto &p = parameters[i];
            native.push_back({p.lower,
                              p.upper,
                              p.initial,
                              static_cast<rfmodel::OptimizationParameterKind>(p.kind)});
        }
        rfmodel::OptimizationOptions opt;
        if (options) {
            require((options->has_target == 0 || options->has_target == 1) &&
                    (options->reject_invalid == 0 || options->reject_invalid == 1) &&
                    (options->record_history == 0 || options->record_history == 1));
            require(options->has_target || options->target == 0.);
            opt.max_evaluations = options->max_evaluations;
            opt.max_iterations = options->max_iterations;
            opt.initial_step = options->initial_step;
            opt.minimum_step = options->minimum_step;
            opt.contraction = options->contraction;
            if (options->has_target) {
                opt.target = options->target;
            }
            opt.invalid_evaluation = options->reject_invalid
                                         ? rfmodel::OptimizationFailurePolicy::Reject
                                         : rfmodel::OptimizationFailurePolicy::Stop;
            opt.record_history = options->record_history != 0;
        }
        auto owned = std::make_unique<rfmodel_optimization_result>();
        owned->core = rfmodel::minimize_bounded(
            native,
            [&](const std::vector<double> &point) {
                double value = std::numeric_limits<double>::quiet_NaN();
                char error[513]{};
                const int status =
                    objective(point.data(), point.size(), user_data, &value, error, sizeof(error));
                error[512] = '\0';
                if (status != 0) {
                    throw std::runtime_error(error[0] ? error : "objective callback failed");
                }
                return value;
            },
            opt,
            [&] {
                if (!cancelled) {
                    return false;
                }
                const int status = cancelled(user_data);
                if (status < 0) {
                    throw std::runtime_error("optimization cancellation callback failed");
                }
                return status > 0;
            });
        // A nested API call inside a callback can replace thread-local error text.
        last_error[0] = '\0';
        *out = owned.release();
    });
}

void rfmodel_optimization_destroy(rfmodel_optimization_result *result) {
    delete result;
}

int rfmodel_optimization_get_summary(const rfmodel_optimization_result *result,
                                     rfmodel_optimization_summary *summary) {
    return guarded([&] {
        require(result && summary);
        disjoint(summary, sizeof(*summary), result, sizeof(*result));
        const auto &r = result->core;
        *summary = {r.parameters.size(),
                    r.history.size(),
                    r.evaluations,
                    r.rejected_evaluations,
                    r.iterations,
                    r.last_failed_parameters.size(),
                    r.last_error.size(),
                    r.has_finite_point ? 1 : 0,
                    static_cast<int>(r.stop),
                    r.value,
                    r.final_step};
    });
}

int rfmodel_optimization_get_parameters(const rfmodel_optimization_result *result,
                                        double *parameters,
                                        size_t capacity) {
    return guarded([&] {
        require(result != nullptr);
        optimization_buffer(
            result, parameters, capacity, result->core.parameters.size(), sizeof(double));
        std::copy(result->core.parameters.begin(), result->core.parameters.end(), parameters);
    });
}

int rfmodel_optimization_get_last_failure(const rfmodel_optimization_result *result,
                                          double *parameters,
                                          size_t capacity,
                                          char *error,
                                          size_t error_capacity) {
    return guarded([&] {
        require(result != nullptr);
        const auto &r = result->core;
        const auto bytes = optimization_buffer(
            result, parameters, capacity, r.last_failed_parameters.size(), sizeof(double));
        const auto error_bytes =
            optimization_buffer(result, error, error_capacity, r.last_error.size() + 1, 1);
        disjoint(parameters, bytes, error, error_bytes);
        if (!r.last_failed_parameters.empty()) {
            std::copy(r.last_failed_parameters.begin(), r.last_failed_parameters.end(), parameters);
        }
        optimization_text(r.last_error, error);
    });
}

int rfmodel_optimization_get_evaluation(const rfmodel_optimization_result *result,
                                        size_t index,
                                        rfmodel_optimization_evaluation *info,
                                        double *parameters,
                                        size_t capacity,
                                        char *error,
                                        size_t error_capacity) {
    return guarded([&] {
        require(result && info && index < result->core.history.size());
        disjoint(info, sizeof(*info), result, sizeof(*result));
        const auto &e = result->core.history[index];
        const rfmodel_optimization_evaluation meta{
            e.value, e.iteration, static_cast<int>(e.status), e.error.size()};
        if (!parameters && capacity == 0 && !error && error_capacity == 0) {
            *info = meta;
            return;
        }
        const auto bytes =
            optimization_buffer(result, parameters, capacity, e.parameters.size(), sizeof(double));
        const auto error_bytes =
            optimization_buffer(result, error, error_capacity, e.error.size() + 1, 1);
        disjoint(info, sizeof(*info), parameters, bytes);
        disjoint(info, sizeof(*info), error, error_bytes);
        disjoint(parameters, bytes, error, error_bytes);
        std::copy(e.parameters.begin(), e.parameters.end(), parameters);
        optimization_text(e.error, error);
        *info = meta;
    });
}

int rfmodel_mix_coherent_components(double spacing_hz,
                                    const rfmodel_coherent_mixer_input *input,
                                    size_t input_count,
                                    uint64_t reserved_group_max,
                                    rfmodel_coherent_component *output,
                                    size_t output_capacity,
                                    size_t *output_count) {
    return guarded([&] {
        require(input_count <= 2048 && output_count && output_capacity >= 2 * input_count);
        require(input_count == 0 || (input && output));
        std::vector<rfmodel::CoherentMixerInput> components;
        components.reserve(input_count);
        for (size_t i = 0; i < input_count; ++i) {
            const auto &entry = input[i];
            const auto &c = entry.component;
            components.push_back({{c.index,
                                   static_cast<rfmodel::SpectrumKind>(c.kind),
                                   c.bandwidth_hz,
                                   c.coherence_group,
                                   {c.amplitude.real, c.amplitude.imag}},
                                  entry.lo_index,
                                  entry.conversion_gain_db,
                                  entry.lo_phase_radians,
                                  entry.lo_coherence_group});
        }
        const auto result =
            rfmodel::mix_coherent_components(spacing_hz, components, reserved_group_max);
        for (size_t i = 0; i < result.size(); ++i) {
            const auto &c = result[i];
            output[i] = {c.bin,
                         static_cast<int>(c.kind),
                         c.bandwidth_hz,
                         c.coherence_group,
                         {c.amplitude.real(), c.amplitude.imag()}};
        }
        *output_count = result.size();
    });
}

int rfmodel_assign_source_coherence(const rfmodel_source_coherence *sources,
                                    size_t count,
                                    uint64_t *groups,
                                    size_t capacity) {
    return guarded([&] {
        require(count <= 4096 && capacity >= count);
        require(count == 0 || (sources && groups));
        auto label = [](const char *value, bool optional) {
            if (!value) {
                require(optional);
                return std::string{};
            }
            size_t length = 0;
            while (length <= 1024 && value[length] != '\0') {
                ++length;
            }
            require(length <= 1024);
            return std::string(value, length);
        };
        std::vector<rfmodel::SourceCoherence> input;
        input.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            input.push_back(
                {label(sources[i].source_id, false), label(sources[i].reference_clock, true)});
        }
        const auto resolved = rfmodel::assign_source_coherence(input);
        for (size_t i = 0; i < count; ++i) {
            groups[i] = resolved[i];
        }
    });
}

int rfmodel_reduce_coherent_components(double spacing_hz,
                                       const rfmodel_coherent_component *input,
                                       size_t input_count,
                                       rfmodel_coherent_component *groups,
                                       size_t group_capacity,
                                       size_t *group_count,
                                       rfmodel_bin_power *powers,
                                       size_t power_capacity,
                                       size_t *power_count,
                                       double *total_power_w) {
    return guarded([&] {
        require(input_count <= 4096 && (input || input_count == 0));
        require(group_count && power_count && total_power_w);
        std::vector<rfmodel::CoherentComponent> components;
        components.reserve(input_count);
        for (size_t i = 0; i < input_count; ++i) {
            const auto &value = input[i];
            components.push_back({value.index,
                                  static_cast<rfmodel::SpectrumKind>(value.kind),
                                  value.bandwidth_hz,
                                  value.coherence_group,
                                  {value.amplitude.real, value.amplitude.imag}});
        }
        const auto result = rfmodel::reduce_coherent_components(spacing_hz, components);
        write_coherence(result,
                        groups,
                        group_capacity,
                        group_count,
                        powers,
                        power_capacity,
                        power_count,
                        total_power_w);
    });
}

int rfmodel_sum_origin_expressions(const rfmodel_origin_expression *parents,
                                   size_t parent_count,
                                   rfmodel_origin_expression_term *terms,
                                   size_t term_capacity,
                                   size_t *term_count,
                                   rfmodel_origin_factor *factors,
                                   size_t factor_capacity,
                                   size_t *factor_count,
                                   rfmodel_complex *total_amplitude) {
    return guarded([&] {
        const auto result =
            rfmodel::sum_origin_expressions(decode_origin_expressions(parents, parent_count));
        encode_origin_expression(result,
                                 terms,
                                 term_capacity,
                                 term_count,
                                 factors,
                                 factor_capacity,
                                 factor_count,
                                 total_amplitude);
    });
}

int rfmodel_product_origin_expressions(const rfmodel_origin_expression *parents,
                                       size_t parent_count,
                                       const int *indices,
                                       size_t index_count,
                                       rfmodel_complex coefficient,
                                       rfmodel_origin_expression_term *terms,
                                       size_t term_capacity,
                                       size_t *term_count,
                                       rfmodel_origin_factor *factors,
                                       size_t factor_capacity,
                                       size_t *factor_count,
                                       rfmodel_complex *total_amplitude) {
    return guarded([&] {
        require(indices && index_count >= 1 && index_count <= rfmodel::maximum_polynomial_order);
        const auto result =
            rfmodel::product_origin_expressions(decode_origin_expressions(parents, parent_count),
                                                std::vector<int>(indices, indices + index_count),
                                                {coefficient.real, coefficient.imag});
        encode_origin_expression(result,
                                 terms,
                                 term_capacity,
                                 term_count,
                                 factors,
                                 factor_capacity,
                                 factor_count,
                                 total_amplitude);
    });
}

int rfmodel_expand_mixing_origin(const rfmodel_mixing_origin *parents,
                                 size_t parent_count,
                                 const int *indices,
                                 size_t index_count,
                                 rfmodel_origin_factor *output,
                                 size_t capacity,
                                 size_t *count) {
    return guarded([&] {
        require(parents && parent_count >= 1 && parent_count <= 4096);
        require(indices && index_count >= 1 && index_count <= rfmodel::maximum_polynomial_order &&
                count);
        std::vector<rfmodel::MixingOrigin> decoded;
        size_t stored = 0;
        for (size_t i = 0; i < parent_count; ++i) {
            const auto &parent = parents[i];
            require(parent.factors && parent.count >= 1 && parent.count <= 256);
            stored += parent.count;
            require(stored <= 65536);
            rfmodel::MixingOrigin origin;
            for (size_t j = 0; j < parent.count; ++j) {
                origin.push_back({parent.factors[j].root_id, parent.factors[j].sign});
            }
            decoded.push_back(std::move(origin));
        }
        const auto result = rfmodel::expand_mixing_origin(
            decoded, std::vector<int>(indices, indices + index_count));
        require(capacity >= result.size() && output);
        for (size_t i = 0; i < result.size(); ++i) {
            output[i] = {result[i].root_id, result[i].sign};
        }
        *count = result.size();
    });
}

int rfmodel_coherent_polynomial_evaluate(double spacing_hz,
                                         const rfmodel_coherent_component *input,
                                         size_t input_count,
                                         const double *voltage_coefficients,
                                         size_t coefficient_count,
                                         double reference_ohms,
                                         uint64_t reserved_group_max,
                                         rfmodel_coherent_component *reduced_inputs,
                                         size_t reduced_capacity,
                                         size_t *reduced_count,
                                         rfmodel_coherent_polynomial_term *terms,
                                         size_t term_capacity,
                                         size_t *term_count) {
    return coherent_polynomial_evaluate_impl<9>(spacing_hz,
                                                input,
                                                input_count,
                                                voltage_coefficients,
                                                coefficient_count,
                                                reference_ohms,
                                                reserved_group_max,
                                                reduced_inputs,
                                                reduced_capacity,
                                                reduced_count,
                                                terms,
                                                term_capacity,
                                                term_count);
}

int rfmodel_coherent_polynomial_evaluate_v2(double spacing_hz,
                                            const rfmodel_coherent_component *input,
                                            size_t input_count,
                                            const double *voltage_coefficients,
                                            size_t coefficient_count,
                                            double reference_ohms,
                                            uint64_t reserved_group_max,
                                            rfmodel_coherent_component *reduced_inputs,
                                            size_t reduced_capacity,
                                            size_t *reduced_count,
                                            rfmodel_coherent_polynomial_term_v2 *terms,
                                            size_t term_capacity,
                                            size_t *term_count) {
    return coherent_polynomial_evaluate_impl<rfmodel::maximum_polynomial_order>(
        spacing_hz,
        input,
        input_count,
        voltage_coefficients,
        coefficient_count,
        reference_ohms,
        reserved_group_max,
        reduced_inputs,
        reduced_capacity,
        reduced_count,
        terms,
        term_capacity,
        term_count);
}

int rfmodel_get_highorder_amplifier_operating_point(double total_input_power_w,
                                                    double power_gain_db,
                                                    double output_p1db_dbm,
                                                    double output_saturation_dbm,
                                                    const double *nonlinear_voltage_coefficients,
                                                    size_t coefficient_count,
                                                    double reference_ohms,
                                                    rfmodel_amplifier_operating_point *output) {
    return guarded([&] {
        require(output && coefficient_count <= 10 &&
                (nonlinear_voltage_coefficients || coefficient_count == 0));
        std::vector<double> coefficients;
        if (coefficient_count) {
            coefficients.assign(nonlinear_voltage_coefficients,
                                nonlinear_voltage_coefficients + coefficient_count);
        }
        const rfmodel::CoherentHighOrderAmplifier model(
            power_gain_db, output_p1db_dbm, output_saturation_dbm, coefficients, reference_ohms);
        const auto point = model.operating_point(total_input_power_w);
        *output = {point.fundamental_amplitude_gain,
                   point.nonlinear_input_scale,
                   point.limited_input_power_w,
                   point.quadratic_voltage_coefficient,
                   point.cubic_voltage_coefficient};
    });
}

int rfmodel_highorder_amplifier_evaluate(double spacing_hz,
                                         const rfmodel_coherent_component *input,
                                         size_t input_count,
                                         double power_gain_db,
                                         double output_p1db_dbm,
                                         double output_saturation_dbm,
                                         const double *nonlinear_voltage_coefficients,
                                         size_t coefficient_count,
                                         double reference_ohms,
                                         uint64_t reserved_group_max,
                                         int propagate_distortion,
                                         rfmodel_coherent_component *reduced_inputs,
                                         size_t reduced_capacity,
                                         size_t *reduced_count,
                                         rfmodel_coherent_polynomial_term_v2 *terms,
                                         size_t term_capacity,
                                         size_t *term_count,
                                         rfmodel_amplifier_drive *drive,
                                         rfmodel_amplifier_operating_point *operating_point) {
    return guarded([&] {
        require(input_count <= 4096 && (input || input_count == 0));
        require(coefficient_count <= 10 &&
                (nonlinear_voltage_coefficients || coefficient_count == 0));
        require(propagate_distortion == 0 || propagate_distortion == 1);
        require(reduced_count && term_count && drive && operating_point);
        std::vector<double> coefficients;
        if (coefficient_count) {
            coefficients.assign(nonlinear_voltage_coefficients,
                                nonlinear_voltage_coefficients + coefficient_count);
        }
        std::vector<rfmodel::CoherentComponent> components;
        for (size_t i = 0; i < input_count; ++i) {
            const auto &c = input[i];
            components.push_back({c.index,
                                  static_cast<rfmodel::SpectrumKind>(c.kind),
                                  c.bandwidth_hz,
                                  c.coherence_group,
                                  {c.amplitude.real, c.amplitude.imag}});
        }
        const rfmodel::CoherentHighOrderAmplifier model(
            power_gain_db, output_p1db_dbm, output_saturation_dbm, coefficients, reference_ohms);
        const auto result =
            model.evaluate(spacing_hz, components, reserved_group_max, propagate_distortion != 0);
        require(reduced_capacity >= result.inputs.size() && term_capacity >= result.terms.size());
        require((reduced_inputs || result.inputs.empty()) && (terms || result.terms.empty()));
        auto encode = [](const rfmodel::CoherentComponent &c) {
            return rfmodel_coherent_component{c.bin,
                                              static_cast<int>(c.kind),
                                              c.bandwidth_hz,
                                              c.coherence_group,
                                              {c.amplitude.real(), c.amplitude.imag()}};
        };
        for (size_t i = 0; i < result.inputs.size(); ++i) {
            reduced_inputs[i] = encode(result.inputs[i]);
        }
        for (size_t i = 0; i < result.terms.size(); ++i) {
            const auto &term = result.terms[i];
            rfmodel_coherent_polynomial_term_v2 encoded{};
            encoded.order = term.order;
            std::copy(term.input_indices.begin(), term.input_indices.end(), encoded.input_indices);
            encoded.component = encode(term.component);
            terms[i] = encoded;
        }
        *reduced_count = result.inputs.size();
        *term_count = result.terms.size();
        *drive = {result.total_input_power_w, result.operating_point.limited_input_power_w};
        const auto &point = result.operating_point;
        *operating_point = {point.fundamental_amplitude_gain,
                            point.nonlinear_input_scale,
                            point.limited_input_power_w,
                            point.quadratic_voltage_coefficient,
                            point.cubic_voltage_coefficient};
    });
}

static int coherent_amplifier_evaluate_impl(double spacing_hz,
                                            const rfmodel_coherent_component *input,
                                            size_t input_count,
                                            double power_gain_db,
                                            double output_p1db_dbm,
                                            double output_saturation_dbm,
                                            double input_ip2_dbm,
                                            double input_ip3_dbm,
                                            double reference_ohms,
                                            uint64_t reserved_group_max,
                                            rfmodel_coherent_component *reduced_inputs,
                                            size_t reduced_capacity,
                                            size_t *reduced_count,
                                            rfmodel_coherent_amplifier_term *terms,
                                            size_t term_capacity,
                                            size_t *term_count,
                                            rfmodel_amplifier_drive *drive,
                                            bool propagate_distortion) {
    return guarded([&] {
        require(input_count <= 4096 && (input || input_count == 0));
        require(reduced_count && term_count && drive);
        std::vector<rfmodel::CoherentComponent> components;
        for (size_t i = 0; i < input_count; ++i) {
            const auto &c = input[i];
            components.push_back({c.index,
                                  static_cast<rfmodel::SpectrumKind>(c.kind),
                                  c.bandwidth_hz,
                                  c.coherence_group,
                                  {c.amplitude.real, c.amplitude.imag}});
        }
        const rfmodel::CoherentLimitedAmplifier model(power_gain_db,
                                                      output_p1db_dbm,
                                                      output_saturation_dbm,
                                                      input_ip2_dbm,
                                                      input_ip3_dbm,
                                                      reference_ohms);
        const auto result = propagate_distortion
                                ? model.evaluate_cascade(spacing_hz, components, reserved_group_max)
                                : model.evaluate(spacing_hz, components, reserved_group_max);
        require(reduced_capacity >= result.inputs.size() && term_capacity >= result.terms.size());
        require((reduced_inputs || result.inputs.empty()) && (terms || result.terms.empty()));
        auto encode = [](const rfmodel::CoherentComponent &c) {
            return rfmodel_coherent_component{c.bin,
                                              static_cast<int>(c.kind),
                                              c.bandwidth_hz,
                                              c.coherence_group,
                                              {c.amplitude.real(), c.amplitude.imag()}};
        };
        for (size_t i = 0; i < result.inputs.size(); ++i) {
            reduced_inputs[i] = encode(result.inputs[i]);
        }
        for (size_t i = 0; i < result.terms.size(); ++i) {
            const auto &t = result.terms[i];
            terms[i] = {t.order,
                        {t.input_indices[0], t.input_indices[1], t.input_indices[2]},
                        encode(t.component)};
        }
        *reduced_count = result.inputs.size();
        *term_count = result.terms.size();
        *drive = {result.total_input_power_w, result.limited_input_power_w};
    });
}

int rfmodel_coherent_amplifier_evaluate(double spacing_hz,
                                        const rfmodel_coherent_component *input,
                                        size_t input_count,
                                        double power_gain_db,
                                        double output_p1db_dbm,
                                        double output_saturation_dbm,
                                        double input_ip2_dbm,
                                        double input_ip3_dbm,
                                        double reference_ohms,
                                        uint64_t reserved_group_max,
                                        rfmodel_coherent_component *reduced_inputs,
                                        size_t reduced_capacity,
                                        size_t *reduced_count,
                                        rfmodel_coherent_amplifier_term *terms,
                                        size_t term_capacity,
                                        size_t *term_count,
                                        rfmodel_amplifier_drive *drive) {
    return coherent_amplifier_evaluate_impl(spacing_hz,
                                            input,
                                            input_count,
                                            power_gain_db,
                                            output_p1db_dbm,
                                            output_saturation_dbm,
                                            input_ip2_dbm,
                                            input_ip3_dbm,
                                            reference_ohms,
                                            reserved_group_max,
                                            reduced_inputs,
                                            reduced_capacity,
                                            reduced_count,
                                            terms,
                                            term_capacity,
                                            term_count,
                                            drive,
                                            false);
}

int rfmodel_coherent_amplifier_cascade(double spacing_hz,
                                       const rfmodel_coherent_component *input,
                                       size_t input_count,
                                       double power_gain_db,
                                       double output_p1db_dbm,
                                       double output_saturation_dbm,
                                       double input_ip2_dbm,
                                       double input_ip3_dbm,
                                       double reference_ohms,
                                       uint64_t reserved_group_max,
                                       rfmodel_coherent_component *reduced_inputs,
                                       size_t reduced_capacity,
                                       size_t *reduced_count,
                                       rfmodel_coherent_amplifier_term *terms,
                                       size_t term_capacity,
                                       size_t *term_count,
                                       rfmodel_amplifier_drive *drive) {
    return coherent_amplifier_evaluate_impl(spacing_hz,
                                            input,
                                            input_count,
                                            power_gain_db,
                                            output_p1db_dbm,
                                            output_saturation_dbm,
                                            input_ip2_dbm,
                                            input_ip3_dbm,
                                            reference_ohms,
                                            reserved_group_max,
                                            reduced_inputs,
                                            reduced_capacity,
                                            reduced_count,
                                            terms,
                                            term_capacity,
                                            term_count,
                                            drive,
                                            true);
}

int rfmodel_compress_coherent_fundamentals(double spacing_hz,
                                           double power_gain_db,
                                           double output_p1db_dbm,
                                           double output_saturation_dbm,
                                           const rfmodel_coherent_component *input,
                                           size_t input_count,
                                           rfmodel_coherent_component *groups,
                                           size_t group_capacity,
                                           size_t *group_count,
                                           rfmodel_bin_power *powers,
                                           size_t power_capacity,
                                           size_t *power_count,
                                           double *output_power_w,
                                           double *input_power_w) {
    return guarded([&] {
        require(input_count <= 4096 && (input || input_count == 0));
        require(group_count && power_count && output_power_w && input_power_w);
        std::vector<rfmodel::CoherentComponent> components;
        components.reserve(input_count);
        for (size_t i = 0; i < input_count; ++i) {
            const auto &value = input[i];
            components.push_back({value.index,
                                  static_cast<rfmodel::SpectrumKind>(value.kind),
                                  value.bandwidth_hz,
                                  value.coherence_group,
                                  {value.amplitude.real, value.amplitude.imag}});
        }
        const rfmodel::SaturatingFundamentalCompression model(
            power_gain_db, output_p1db_dbm, output_saturation_dbm);
        const auto result = rfmodel::compress_coherent_fundamentals(spacing_hz, components, model);
        write_coherence(result.output,
                        groups,
                        group_capacity,
                        group_count,
                        powers,
                        power_capacity,
                        power_count,
                        output_power_w);
        *input_power_w = result.input_power_w;
    });
}

int rfmodel_network_transmit_coherent(const rfmodel_network *network,
                                      const size_t *external_ports,
                                      size_t port_count,
                                      size_t output_port,
                                      double spacing_hz,
                                      const rfmodel_port_coherent_component *input,
                                      size_t input_count,
                                      rfmodel_coherent_component *groups,
                                      size_t group_capacity,
                                      size_t *group_count,
                                      rfmodel_bin_power *powers,
                                      size_t power_capacity,
                                      size_t *power_count,
                                      double *total_power_w) {
    return guarded([&] {
        require(network && external_ports && port_count > 0 && port_count <= 1024);
        require(input_count <= 4096 && (input || input_count == 0));
        require(group_count && power_count && total_power_w);
        std::vector<rfmodel::PortCoherentComponent> incident;
        incident.reserve(input_count);
        for (size_t i = 0; i < input_count; ++i) {
            const auto &value = input[i].component;
            incident.push_back({input[i].input_port,
                                {value.index,
                                 static_cast<rfmodel::SpectrumKind>(value.kind),
                                 value.bandwidth_hz,
                                 value.coherence_group,
                                 {value.amplitude.real, value.amplitude.imag}}});
        }
        const auto result = rfmodel::transmit_coherent_network(
            spacing_hz,
            incident,
            std::vector<size_t>(external_ports, external_ports + port_count),
            output_port,
            network->core.reference_impedance_ohms(),
            [&](double) {
                return network->core;
            });
        write_coherence(result,
                        groups,
                        group_capacity,
                        group_count,
                        powers,
                        power_capacity,
                        power_count,
                        total_power_w);
    });
}

const char *rfmodel_last_error(void) {
    return last_error;
}

unsigned int rfmodel_abi_version(void) {
    return 1;
}

int rfmodel_saturating_amplitude_gain(double total_incident_power_w,
                                      double power_gain_db,
                                      double output_p1db_dbm,
                                      double output_saturation_dbm,
                                      double *output) {
    return guarded([&] {
        require(output);
        const rfmodel::SaturatingFundamentalCompression model(
            power_gain_db, output_p1db_dbm, output_saturation_dbm);
        const auto result = model.amplitude_gain(total_incident_power_w);
        *output = result;
    });
}

int rfmodel_get_amplifier_operating_point(double total_incident_power_w,
                                          double power_gain_db,
                                          double output_p1db_dbm,
                                          double output_saturation_dbm,
                                          double input_ip2_dbm,
                                          double input_ip3_dbm,
                                          double reference_ohms,
                                          rfmodel_amplifier_operating_point *output) {
    return guarded([&] {
        require(output);
        const rfmodel::CoherentLimitedAmplifier model(power_gain_db,
                                                      output_p1db_dbm,
                                                      output_saturation_dbm,
                                                      input_ip2_dbm,
                                                      input_ip3_dbm,
                                                      reference_ohms);
        const auto point = model.operating_point(total_incident_power_w);
        *output = {point.fundamental_amplitude_gain,
                   point.nonlinear_input_scale,
                   point.limited_input_power_w,
                   point.quadratic_voltage_coefficient,
                   point.cubic_voltage_coefficient};
    });
}

int rfmodel_saturating_fundamental(double power_gain_db,
                                   double output_p1db_dbm,
                                   double output_saturation_dbm,
                                   rfmodel_complex incident,
                                   double total_incident_power_w,
                                   rfmodel_complex *output) {
    return guarded([&] {
        require(output != nullptr);
        const rfmodel::SaturatingFundamentalCompression model(
            power_gain_db, output_p1db_dbm, output_saturation_dbm);
        const auto result =
            model.transmit_fundamental({incident.real, incident.imag}, total_incident_power_w);
        *output = {result.real(), result.imag()};
    });
}

int rfmodel_p1db_fundamental(double power_gain_db,
                             double output_p1db_dbm,
                             rfmodel_complex incident,
                             rfmodel_complex *output) {
    return guarded([&] {
        require(output != nullptr);
        const rfmodel::P1dBFundamentalCompression model(power_gain_db, output_p1db_dbm);
        const auto result = model.transmit_fundamental({incident.real, incident.imag});
        *output = {result.real(), result.imag()};
    });
}

int rfmodel_p1db_driven_fundamental(double power_gain_db,
                                    double output_p1db_dbm,
                                    rfmodel_complex incident,
                                    double total_incident_power_w,
                                    rfmodel_complex *output) {
    return guarded([&] {
        require(output != nullptr);
        const rfmodel::P1dBFundamentalCompression model(power_gain_db, output_p1db_dbm);
        const auto result =
            model.transmit_fundamental({incident.real, incident.imag}, total_incident_power_w);
        *output = {result.real(), result.imag()};
    });
}

int rfmodel_p1db_spectral_fundamental(double power_gain_db,
                                      double output_p1db_dbm,
                                      const rfmodel_incident_spectrum *ports,
                                      size_t port_count,
                                      size_t fundamental_port,
                                      int fundamental_bin,
                                      rfmodel_complex *output,
                                      double *total_incident_power_w) {
    return guarded([&] {
        require(ports && port_count > 0 && port_count <= 1024 && output && total_incident_power_w);
        std::vector<rfmodel::PowerWaveSpectrum> inputs;
        inputs.reserve(port_count);
        for (size_t port = 0; port < port_count; ++port) {
            inputs.push_back(
                read_spectrum(ports[port].spacing_hz, ports[port].bins, ports[port].count));
        }
        const rfmodel::P1dBFundamentalCompression model(power_gain_db, output_p1db_dbm);
        const auto result =
            model.transmit_fundamental_from_spectra(inputs, fundamental_port, fundamental_bin);
        const double total = rfmodel::incident_rf_power_watts(inputs);
        *output = {result.real(), result.imag()};
        *total_incident_power_w = total;
    });
}

int rfmodel_touchstone_open(const char *path_utf8, int out_of_band, rfmodel_touchstone **out) {
    return guarded([&] {
        require(out != nullptr);
        *out = nullptr;
        require(path_utf8 && path_utf8[0] && (out_of_band == 0 || out_of_band == 1));
        const auto policy =
            out_of_band == 0 ? rfmodel::OutOfBand::Reject : rfmodel::OutOfBand::Clamp;
        auto data = rfmodel::read_touchstone(path_utf8);
        rfmodel::common_reference_impedance(data);
        *out = new rfmodel_touchstone(std::move(data), policy);
    });
}

void rfmodel_touchstone_close(rfmodel_touchstone *model) {
    delete model;
}

int rfmodel_touchstone_get_info(const rfmodel_touchstone *model, rfmodel_touchstone_info *info) {
    return guarded([&] {
        require(model && info);
        *info = {model->core.port_count(),
                 model->core.port(0).reference_impedance.real(),
                 model->core.minimum_frequency_hz(),
                 model->core.maximum_frequency_hz(),
                 model->noise_samples};
    });
}

int rfmodel_touchstone_noise(const rfmodel_touchstone *model,
                             double frequency_hz,
                             double reference_ohms,
                             double reference_temperature_k,
                             rfmodel_complex *values,
                             size_t capacity) {
    return guarded([&] {
        require(model && values);
        const auto ports = model->core.port_count();
        require(capacity >= ports * ports);
        const auto noisy = rfmodel::TabulatedNoiseModel::from_data(
            "Touchstone noise C API", model->core.data(), model->policy, reference_temperature_k);
        const auto covariance = rfmodel::renormalize_noise(noisy.s_parameters(frequency_hz),
                                                           noisy.noise_correlation(frequency_hz),
                                                           noisy.port(0).reference_impedance.real(),
                                                           reference_ohms)
                                    .watts_per_hz;
        for (size_t i = 0; i < covariance.values.size(); ++i) {
            values[i] = {covariance.values[i].real(), covariance.values[i].imag()};
        }
    });
}

int rfmodel_touchstone_s(const rfmodel_touchstone *model,
                         double frequency_hz,
                         double reference_ohms,
                         rfmodel_complex *values,
                         size_t capacity) {
    return guarded([&] {
        require(model && values);
        const auto ports = model->core.port_count();
        require(capacity >= ports * ports);
        const auto scattering =
            rfmodel::renormalize_s(model->core.s_parameters(frequency_hz),
                                   model->core.port(0).reference_impedance.real(),
                                   reference_ohms);
        for (size_t i = 0; i < scattering.values.size(); ++i) {
            values[i] = {scattering.values[i].real(), scattering.values[i].imag()};
        }
    });
}

int rfmodel_network_transmit_spectrum(const rfmodel_network *network,
                                      const size_t *external_ports,
                                      size_t external_count,
                                      double spacing_hz,
                                      const rfmodel_spectrum_bin *input,
                                      size_t input_count,
                                      rfmodel_spectrum_bin *output,
                                      size_t capacity,
                                      size_t *output_count) {
    return guarded([&] {
        require(network && external_ports && external_count == 2 && output_count);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const std::vector<size_t> selection(external_ports, external_ports + external_count);
        // Validate the topology even for empty input; no silently accepted bad ports.
        network->core.external_s(selection);
        const auto transmitted = rfmodel::transmit_linear_spectrum(
            incident, selection, network->core.reference_impedance_ohms(), [&](double) {
                return network->core;
            });
        write_spectrum(transmitted, output, capacity, output_count);
    });
}

int rfmodel_cubic_amplifier_transmit(double spacing_hz,
                                     const rfmodel_spectrum_bin *input,
                                     size_t input_count,
                                     double power_gain_db,
                                     double input_ip3_dbm,
                                     double reference_ohms,
                                     rfmodel_spectrum_bin *output,
                                     size_t capacity,
                                     size_t *output_count) {
    return guarded([&] {
        require(output_count != nullptr);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const auto model = rfmodel::MatchedPolynomialAmplifier::from_iip3(
            "C API cubic amplifier", power_gain_db, input_ip3_dbm, reference_ohms);
        write_spectrum(model.transmit(incident), output, capacity, output_count);
    });
}

int rfmodel_polynomial_coefficients_from_intermod_levels(double power_gain_db,
                                                         const double *output_levels_dbm,
                                                         size_t level_count,
                                                         const int *coefficient_signs,
                                                         size_t sign_count,
                                                         double reference_ohms,
                                                         double *coefficients,
                                                         size_t capacity,
                                                         size_t *coefficient_count) {
    return guarded([&] {
        require(output_levels_dbm && level_count >= 1 &&
                level_count <= rfmodel::maximum_polynomial_order);
        require(sign_count == level_count - 1 && (coefficient_signs || sign_count == 0));
        require(coefficients && coefficient_count && capacity >= 2);
        require(capacity <= std::numeric_limits<size_t>::max() / sizeof(double));
        const std::array<const void *, 4> pointers{
            output_levels_dbm, coefficient_signs, coefficients, coefficient_count};
        const std::array<size_t, 4> sizes{level_count * sizeof(double),
                                          sign_count * sizeof(int),
                                          capacity * sizeof(double),
                                          sizeof(size_t)};
        for (size_t i = 0; i < pointers.size(); ++i) {
            for (size_t j = i + 1; j < pointers.size(); ++j) {
                disjoint(pointers[i], sizes[i], pointers[j], sizes[j]);
            }
        }
        std::vector<int> signs;
        if (sign_count) {
            signs.assign(coefficient_signs, coefficient_signs + sign_count);
        }
        const auto result = rfmodel::polynomial_coefficients_from_intermod_levels(
            power_gain_db,
            std::vector<double>(output_levels_dbm, output_levels_dbm + level_count),
            signs,
            reference_ohms);
        require(capacity >= result.size());
        std::copy(result.begin(), result.end(), coefficients);
        *coefficient_count = result.size();
    });
}

int rfmodel_polynomial_coefficients_from_intercepts(double power_gain_db,
                                                    const rfmodel_two_tone_intercept *intercepts,
                                                    size_t intercept_count,
                                                    double reference_ohms,
                                                    double *coefficients,
                                                    size_t capacity,
                                                    size_t *coefficient_count) {
    return guarded([&] {
        require(intercept_count <= rfmodel::maximum_polynomial_order - 1 &&
                (intercepts || intercept_count == 0));
        require(coefficients && coefficient_count && capacity >= 2);
        require(capacity <= std::numeric_limits<size_t>::max() / sizeof(double));
        disjoint(intercepts,
                 intercept_count * sizeof(*intercepts),
                 coefficients,
                 capacity * sizeof(double));
        disjoint(intercepts,
                 intercept_count * sizeof(*intercepts),
                 coefficient_count,
                 sizeof(*coefficient_count));
        disjoint(
            coefficients, capacity * sizeof(double), coefficient_count, sizeof(*coefficient_count));
        std::vector<rfmodel::TwoToneIntercept> decoded;
        for (size_t i = 0; i < intercept_count; ++i) {
            const auto &value = intercepts[i];
            require(value.reference == RFMODEL_INTERCEPT_INPUT ||
                    value.reference == RFMODEL_INTERCEPT_OUTPUT);
            decoded.push_back({value.first_tone_order,
                               value.second_tone_order,
                               value.intercept_dbm,
                               value.coefficient_sign,
                               static_cast<rfmodel::InterceptReference>(value.reference)});
        }
        const auto result = rfmodel::polynomial_coefficients_from_intercepts(
            power_gain_db, decoded, reference_ohms);
        require(capacity >= result.size());
        std::copy(result.begin(), result.end(), coefficients);
        *coefficient_count = result.size();
    });
}

int rfmodel_polynomial_amplifier_transmit(double spacing_hz,
                                          const rfmodel_spectrum_bin *input,
                                          size_t input_count,
                                          const double *coefficients,
                                          size_t coefficient_count,
                                          double reference_ohms,
                                          rfmodel_spectrum_bin *output,
                                          size_t capacity,
                                          size_t *output_count) {
    return guarded([&] {
        require(output_count && coefficients && coefficient_count > 0 &&
                coefficient_count <= rfmodel::maximum_polynomial_order + 1);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const rfmodel::MatchedPolynomialAmplifier model(
            "C API polynomial amplifier",
            std::vector<double>(coefficients, coefficients + coefficient_count),
            reference_ohms);
        write_spectrum(model.transmit(incident), output, capacity, output_count);
    });
}

int rfmodel_intercept_amplifier_transmit(double spacing_hz,
                                         const rfmodel_spectrum_bin *input,
                                         size_t input_count,
                                         double power_gain_db,
                                         double input_ip2_dbm,
                                         double input_ip3_dbm,
                                         double reference_ohms,
                                         rfmodel_spectrum_bin *output,
                                         size_t capacity,
                                         size_t *output_count) {
    return guarded([&] {
        require(output_count != nullptr);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const auto model =
            rfmodel::MatchedPolynomialAmplifier::from_intercepts("C API intercept amplifier",
                                                                 power_gain_db,
                                                                 input_ip2_dbm,
                                                                 input_ip3_dbm,
                                                                 reference_ohms);
        write_spectrum(model.transmit(incident), output, capacity, output_count);
    });
}

int rfmodel_single_tone_amplifier_transmit(double spacing_hz,
                                           const rfmodel_spectrum_bin *input,
                                           size_t input_count,
                                           double power_gain_db,
                                           double output_p1db_dbm,
                                           double output_saturation_dbm,
                                           double input_ip2_dbm,
                                           double input_ip3_dbm,
                                           double reference_ohms,
                                           rfmodel_spectrum_bin *output,
                                           size_t capacity,
                                           size_t *output_count) {
    return guarded([&] {
        require(output_count != nullptr);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const rfmodel::SingleToneLimitedAmplifier model(power_gain_db,
                                                        output_p1db_dbm,
                                                        output_saturation_dbm,
                                                        input_ip2_dbm,
                                                        input_ip3_dbm,
                                                        reference_ohms);
        write_spectrum(model.transmit(incident), output, capacity, output_count);
    });
}

int rfmodel_multitone_amplifier_evaluate(double spacing_hz,
                                         const rfmodel_spectrum_bin *input,
                                         size_t input_count,
                                         double power_gain_db,
                                         double output_p1db_dbm,
                                         double output_saturation_dbm,
                                         double input_ip2_dbm,
                                         double input_ip3_dbm,
                                         double reference_ohms,
                                         rfmodel_amplifier_component *output,
                                         size_t capacity,
                                         size_t *output_count,
                                         rfmodel_amplifier_drive *drive) {
    return guarded([&] {
        require(output_count && drive);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const rfmodel::MultiToneLimitedAmplifier model(power_gain_db,
                                                       output_p1db_dbm,
                                                       output_saturation_dbm,
                                                       input_ip2_dbm,
                                                       input_ip3_dbm,
                                                       reference_ohms);
        const auto response = model.evaluate(incident);
        const auto required = response.direct.amplitudes.size() +
                              response.second_order.amplitudes.size() +
                              response.third_order.amplitudes.size();
        require(capacity >= required && (output || required == 0));
        const rfmodel::PowerWaveSpectrum *families[] = {
            &response.direct, &response.second_order, &response.third_order};
        size_t written = 0;
        for (int order = 1; order <= 3; ++order) {
            for (const auto &entry : families[order - 1]->amplitudes) {
                output[written++] = {
                    order, entry.first, {entry.second.real(), entry.second.imag()}};
            }
        }
        *output_count = written;
        *drive = {response.total_input_power_w, response.limited_input_power_w};
    });
}

int rfmodel_network_transmit_terms(const rfmodel_network *network,
                                   const size_t *external_ports,
                                   size_t external_count,
                                   double spacing_hz,
                                   const rfmodel_amplifier_term *input,
                                   size_t input_count,
                                   rfmodel_amplifier_term *output,
                                   size_t capacity,
                                   size_t *output_count) {
    return guarded([&] {
        require(network && external_ports && external_count == 2 && output_count &&
                input_count <= 4096 && (input || input_count == 0) && capacity >= input_count &&
                (output || input_count == 0));
        std::vector<rfmodel::AmplifierMixingTerm> terms;
        for (size_t i = 0; i < input_count; ++i) {
            const auto &term = input[i];
            terms.push_back({term.order,
                             term.index,
                             {term.contributors[0], term.contributors[1], term.contributors[2]},
                             {term.amplitude.real, term.amplitude.imag}});
        }
        const std::vector<size_t> ports(external_ports, external_ports + external_count);
        const auto result = rfmodel::transmit_linear_terms(
            spacing_hz, terms, ports, network->core.reference_impedance_ohms(), [&](double) {
                return network->core;
            });
        for (size_t i = 0; i < result.size(); ++i) {
            const auto &term = result[i];
            output[i] = {term.order,
                         term.bin,
                         {term.contributors[0], term.contributors[1], term.contributors[2]},
                         {term.amplitude.real(), term.amplitude.imag()}};
        }
        *output_count = result.size();
    });
}

int rfmodel_multitone_amplifier_terms(double spacing_hz,
                                      const rfmodel_spectrum_bin *input,
                                      size_t input_count,
                                      double power_gain_db,
                                      double output_p1db_dbm,
                                      double output_saturation_dbm,
                                      double input_ip2_dbm,
                                      double input_ip3_dbm,
                                      double reference_ohms,
                                      rfmodel_amplifier_term *output,
                                      size_t capacity,
                                      size_t *output_count,
                                      rfmodel_amplifier_drive *drive) {
    return guarded([&] {
        require(output_count && drive);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const rfmodel::MultiToneLimitedAmplifier model(power_gain_db,
                                                       output_p1db_dbm,
                                                       output_saturation_dbm,
                                                       input_ip2_dbm,
                                                       input_ip3_dbm,
                                                       reference_ohms);
        const auto response = model.evaluate_terms(incident);
        require(capacity >= response.terms.size() && (output || response.terms.empty()));
        size_t written = 0;
        for (const auto &term : response.terms) {
            output[written++] = {term.order,
                                 term.bin,
                                 {term.contributors[0], term.contributors[1], term.contributors[2]},
                                 {term.amplitude.real(), term.amplitude.imag()}};
        }
        *output_count = written;
        *drive = {response.total_input_power_w, response.limited_input_power_w};
    });
}

int rfmodel_ideal_mixer_transmit(double spacing_hz,
                                 const rfmodel_spectrum_bin *input,
                                 size_t input_count,
                                 int lo_bin,
                                 double conversion_gain_db,
                                 double lo_phase_radians,
                                 double reference_ohms,
                                 rfmodel_spectrum_bin *output,
                                 size_t capacity,
                                 size_t *output_count) {
    return guarded([&] {
        require(output_count != nullptr);
        const auto incident = read_spectrum(spacing_hz, input, input_count);
        const rfmodel::IdealRealMixer model(
            "C API mixer", lo_bin, conversion_gain_db, lo_phase_radians, reference_ohms);
        write_spectrum(model.transmit(incident), output, capacity, output_count);
    });
}

int rfmodel_network_reduce_power_waves(const rfmodel_network *network,
                                       const size_t *ports,
                                       size_t port_count,
                                       double reference_ohms,
                                       rfmodel_network **out) {
    return guarded([&] {
        require(network && ports && port_count > 0 && port_count <= 1024 && out);
        disjoint(out, sizeof(*out), network, sizeof(*network));
        disjoint(out, sizeof(*out), ports, port_count * sizeof(*ports));
        require(*out == nullptr);
        auto core = rfmodel::reduce_power_wave_network(
            network->core, std::vector<size_t>(ports, ports + port_count), reference_ohms);
        auto result = std::make_unique<rfmodel_network>(reference_ohms);
        result->core = std::move(core);
        result->ports = port_count;
        *out = result.release();
    });
}

int rfmodel_network_create(double reference, rfmodel_network **out) {
    return guarded([&] {
        require(out != nullptr);
        *out = nullptr;
        *out = new rfmodel_network(reference);
    });
}

void rfmodel_network_destroy(rfmodel_network *network) {
    delete network;
}

int rfmodel_network_add(rfmodel_network *network,
                        size_t ports,
                        const rfmodel_complex *values,
                        size_t value_count,
                        double reference,
                        size_t *first_port) {
    return guarded([&] {
        require(network && values && first_port && ports > 0 && ports <= 1024);
        require(value_count == ports * ports);
        rfmodel::SMatrix matrix{ports, std::vector<rfmodel::Complex>(value_count)};
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {values[i].real, values[i].imag};
        }
        // Commit only after success, including allocation failures inside core.add().
        auto updated = network->core;
        const auto offset = updated.add(matrix, reference);
        network->core = std::move(updated);
        network->ports += ports;
        *first_port = offset;
    });
}

int rfmodel_network_add_power_waves(rfmodel_network *network,
                                    size_t ports,
                                    const rfmodel_complex *values,
                                    size_t value_count,
                                    const rfmodel_complex *references,
                                    size_t reference_count,
                                    size_t *first_port) {
    return guarded([&] {
        require(network && values && references && first_port && ports > 0 && ports <= 1024);
        require(value_count == ports * ports && reference_count == ports);
        disjoint(first_port, sizeof(*first_port), values, value_count * sizeof(*values));
        disjoint(
            first_port, sizeof(*first_port), references, reference_count * sizeof(*references));
        disjoint(first_port, sizeof(*first_port), network, sizeof(*network));
        rfmodel::SMatrix matrix{ports, std::vector<rfmodel::Complex>(value_count)};
        std::vector<rfmodel::Complex> refs(ports);
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {values[i].real, values[i].imag};
        }
        for (size_t i = 0; i < ports; ++i) {
            refs[i] = {references[i].real, references[i].imag};
        }
        auto updated = network->core;
        const auto offset = updated.add_power_waves(matrix, refs);
        network->core = std::move(updated);
        network->ports += ports;
        *first_port = offset;
    });
}

int rfmodel_network_get_references(const rfmodel_network *network,
                                   rfmodel_complex *references,
                                   size_t capacity) {
    return guarded([&] {
        require(network && capacity >= network->ports);
        require(references || (capacity == 0 && network->ports == 0));
        require(capacity <= std::numeric_limits<size_t>::max() / sizeof(*references));
        disjoint(references, capacity * sizeof(*references), network, sizeof(*network));
        for (size_t i = 0; i < network->ports; ++i) {
            const auto z = network->core.port_reference_impedance_ohms(i);
            references[i] = {z.real(), z.imag()};
        }
    });
}

int rfmodel_network_connect(rfmodel_network *network, size_t first, size_t second) {
    return guarded([&] {
        require(network != nullptr);
        network->core.connect(first, second);
    });
}

int rfmodel_network_terminate(rfmodel_network *network,
                              size_t port,
                              rfmodel_complex reflection,
                              rfmodel_complex source) {
    return guarded([&] {
        require(network != nullptr);
        network->core.terminate(
            port, {reflection.real, reflection.imag}, {source.real, source.imag});
    });
}

int rfmodel_network_port_count(const rfmodel_network *network, size_t *count) {
    return guarded([&] {
        require(network && count);
        *count = network->ports;
    });
}

int rfmodel_network_solve(const rfmodel_network *network,
                          rfmodel_complex *incident,
                          rfmodel_complex *outgoing,
                          size_t capacity,
                          double *relative_residual) {
    return guarded([&] {
        require(network && incident && outgoing && relative_residual);
        require(capacity >= network->ports);
        const auto waves = network->core.solve();
        for (size_t i = 0; i < network->ports; ++i) {
            incident[i] = {waves.incident[i].real(), waves.incident[i].imag()};
            outgoing[i] = {waves.outgoing[i].real(), waves.outgoing[i].imag()};
        }
        *relative_residual = waves.relative_residual;
    });
}

int rfmodel_network_external_s(const rfmodel_network *network,
                               const size_t *ports,
                               size_t port_count,
                               rfmodel_complex *values,
                               size_t capacity) {
    return guarded([&] {
        require(network && ports && values && port_count > 0 && port_count <= 1024);
        require(capacity >= port_count * port_count);
        const auto matrix =
            network->core.external_s(std::vector<size_t>(ports, ports + port_count));
        for (size_t i = 0; i < matrix.values.size(); ++i) {
            values[i] = {matrix.values[i].real(), matrix.values[i].imag()};
        }
    });
}

int rfmodel_transmission_line_s(double frequency_hz,
                                double characteristic_ohms,
                                double delay_s,
                                double propagation_loss_db,
                                double reference_ohms,
                                rfmodel_complex *values,
                                size_t capacity) {
    return guarded([&] {
        require(values && capacity >= 4);
        const rfmodel::TransmissionLineModel model(
            "C API line", characteristic_ohms, delay_s, propagation_loss_db, reference_ohms);
        const auto matrix = model.s_parameters(frequency_hz);
        for (size_t i = 0; i < 4; ++i) {
            values[i] = {matrix.values[i].real(), matrix.values[i].imag()};
        }
    });
}

int rfmodel_rlgc_line_s(double frequency_hz,
                        double resistance_ohms_per_m,
                        double inductance_h_per_m,
                        double conductance_s_per_m,
                        double capacitance_f_per_m,
                        double length_m,
                        double reference_ohms,
                        rfmodel_complex *values,
                        size_t capacity) {
    return guarded([&] {
        require(values && capacity >= 4);
        const rfmodel::RlgcTransmissionLineModel model(
            "C API RLGC line",
            {resistance_ohms_per_m, inductance_h_per_m, conductance_s_per_m, capacitance_f_per_m},
            length_m,
            reference_ohms);
        const auto matrix = model.s_parameters(frequency_hz);
        for (size_t i = 0; i < 4; ++i) {
            values[i] = {matrix.values[i].real(), matrix.values[i].imag()};
        }
    });
}

int rfmodel_linear_amplifier_s(double frequency_hz,
                               double gain_db,
                               double gain_phase_degrees,
                               double reverse_isolation_db,
                               double reverse_phase_degrees,
                               rfmodel_complex input_impedance_ohms,
                               rfmodel_complex output_impedance_ohms,
                               double reference_ohms,
                               rfmodel_complex *values,
                               size_t capacity) {
    return guarded([&] {
        require(values && capacity >= 4);
        rfmodel::LinearAmplifierParameters parameters;
        parameters.gain_db = gain_db;
        parameters.gain_phase_degrees = gain_phase_degrees;
        parameters.reverse_isolation_db = reverse_isolation_db;
        parameters.reverse_phase_degrees = reverse_phase_degrees;
        parameters.input_impedance_ohms = {input_impedance_ohms.real, input_impedance_ohms.imag};
        parameters.output_impedance_ohms = {output_impedance_ohms.real, output_impedance_ohms.imag};
        parameters.reference_ohms = reference_ohms;
        const auto matrix =
            rfmodel::LinearAmplifierModel("C API amplifier", parameters).s_parameters(frequency_hz);
        for (size_t i = 0; i < 4; ++i) {
            values[i] = {matrix.values[i].real(), matrix.values[i].imag()};
        }
    });
}

int rfmodel_phase_noise_group(const size_t *physical_ports,
                              const int *bins,
                              size_t count,
                              const rfmodel_phase_noise_carrier *carriers,
                              size_t carrier_count,
                              const int *offset_bins,
                              const double *ssb_dbc_per_hz,
                              size_t offset_count,
                              rfmodel_complex *covariance,
                              rfmodel_complex *complementary,
                              size_t matrix_capacity) {
    return guarded([&] {
        require(count && count <= 512 && carrier_count && carrier_count <= count && carriers &&
                offset_count && offset_count <= 255 && physical_ports && bins && offset_bins &&
                ssb_dbc_per_hz && covariance && complementary && matrix_capacity >= count * count);
        const auto bytes = count * count * sizeof(rfmodel_complex);
        disjoint(covariance, bytes, complementary, bytes);
        for (auto *output : {covariance, complementary}) {
            disjoint(output, bytes, physical_ports, count * sizeof(size_t));
            disjoint(output, bytes, bins, count * sizeof(int));
            disjoint(output, bytes, carriers, carrier_count * sizeof(*carriers));
            disjoint(output, bytes, offset_bins, offset_count * sizeof(int));
            disjoint(output, bytes, ssb_dbc_per_hz, offset_count * sizeof(double));
        }
        std::vector<rfmodel::ConversionChannel> channels;
        std::vector<rfmodel::PhaseNoiseOffset> offsets;
        std::vector<rfmodel::PhaseNoiseCarrier> members;
        for (size_t i = 0; i < count; ++i) {
            channels.push_back({physical_ports[i], bins[i]});
        }
        for (size_t i = 0; i < offset_count; ++i) {
            offsets.push_back({offset_bins[i], ssb_dbc_per_hz[i]});
        }
        for (size_t i = 0; i < carrier_count; ++i) {
            members.push_back({carriers[i].channel,
                               {carriers[i].wave.real, carriers[i].wave.imag},
                               carriers[i].phase_gain});
        }
        const auto result = rfmodel::phase_noise_group(channels, members, offsets);
        for (size_t i = 0; i < count * count; ++i) {
            covariance[i] = {result.covariance.values[i].real(),
                             result.covariance.values[i].imag()};
            complementary[i] = {result.complementary.values[i].real(),
                                result.complementary.values[i].imag()};
        }
    });
}

int rfmodel_phase_noise_sidebands(const size_t *physical_ports,
                                  const int *bins,
                                  size_t count,
                                  size_t carrier_channel,
                                  rfmodel_complex carrier_wave,
                                  const int *offset_bins,
                                  const double *ssb_dbc_per_hz,
                                  size_t offset_count,
                                  rfmodel_complex *covariance,
                                  rfmodel_complex *complementary,
                                  size_t matrix_capacity) {
    const rfmodel_phase_noise_carrier carrier{carrier_channel, carrier_wave, 1.};
    return rfmodel_phase_noise_group(physical_ports,
                                     bins,
                                     count,
                                     &carrier,
                                     1,
                                     offset_bins,
                                     ssb_dbc_per_hz,
                                     offset_count,
                                     covariance,
                                     complementary,
                                     matrix_capacity);
}

int rfmodel_linearize_real_mixer(const rfmodel_mixer_linearization_request *request,
                                 const rfmodel_mixer_linearization_output *output) {
    return guarded([&] {
        linearize_conversion_outputs(
            request, output, [](const auto &r, const auto &channels, const auto &waves) {
                const auto point = rfmodel::linearize_real_mixer(r.spacing_hz,
                                                                 channels,
                                                                 waves,
                                                                 r.lo_bin,
                                                                 r.gain_db,
                                                                 r.rf_port,
                                                                 r.lo_port,
                                                                 r.if_port,
                                                                 r.reference_ohms);
                return rfmodel::ConversionLinearization{point.incremental_model,
                                                        point.operating_outgoing};
            });
    });
}

int rfmodel_linearize_bilinear_mixer(const rfmodel_bilinear_mixer_request *request,
                                     const rfmodel_mixer_linearization_output *output) {
    return guarded([&] {
        linearize_conversion_outputs(
            request, output, [](const auto &r, const auto &channels, const auto &waves) {
                const auto &m = r.model;
                const auto point = rfmodel::linearize_bilinear_mixer(r.spacing_hz,
                                                                     channels,
                                                                     waves,
                                                                     m.gain_db,
                                                                     m.lo_reference_amplitude,
                                                                     m.rf_port,
                                                                     m.lo_port,
                                                                     m.if_port,
                                                                     r.reference_ohms);
                return rfmodel::ConversionLinearization{point.incremental_model,
                                                        point.operating_outgoing};
            });
    });
}

int rfmodel_linearize_saturating_amplifier(const rfmodel_saturating_amplifier_request *request,
                                           const rfmodel_conversion_linearization_output *output) {
    return guarded([&] {
        linearize_conversion_outputs(
            request, output, [](const auto &r, const auto &channels, const auto &waves) {
                return amplifier_point(r.spacing_hz, channels, waves, r.model, r.reference_ohms);
            });
    });
}

int rfmodel_evaluate_am_pm(double peak,
                           double reference_dbm,
                           double power,
                           rfmodel_am_pm_response *output) {
    return guarded([&] {
        require(output);
        const auto value = rfmodel::LegacyPointDerivativeAmPm(peak, reference_dbm).response(power);
        *output = {value.phase_rad, value.radial_phase_slope};
    });
}

int rfmodel_apply_output_am_pm(const rfmodel_am_pm_linearization_request *request,
                               const rfmodel_conversion_linearization_output *output) {
    return guarded([&] {
        require(request && request->count > 0 && request->count <= 512 && request->direct &&
                request->conjugate);
        const auto &r = *request;
        const auto range = am_pm_range(r.phase);
        rfmodel_saturating_amplifier_request proxy{};
        proxy.count = r.count;
        proxy.spacing_hz = r.spacing_hz;
        proxy.reference_ohms = r.reference_ohms;
        proxy.physical_ports = r.physical_ports;
        proxy.bins = r.bins;
        proxy.operating_incident = r.operating_outgoing;
        linearize_conversion_outputs(
            &proxy,
            output,
            [&](const auto &, const auto &channels, const auto &waves) {
                auto a = rfmodel::conversion_detail::zero(r.count), b = a;
                for (size_t i = 0; i < r.count * r.count; ++i) {
                    a.values[i] = {r.direct[i].real, r.direct[i].imag};
                    b.values[i] = {r.conjugate[i].real, r.conjugate[i].imag};
                }
                const rfmodel::ConversionLinearization base{
                    rfmodel::FrequencyConversionModel(
                        r.spacing_hz, channels, a, b, r.reference_ohms),
                    waves};
                return rfmodel::apply_output_am_pm(
                    base,
                    rfmodel::LegacyPointDerivativeAmPm(r.phase.peak_degrees_per_db,
                                                       r.phase.output_reference_dbm),
                    am_pm_channels(r.phase));
            },
            {{request, sizeof(*request)},
             {r.direct, r.count * r.count * sizeof(rfmodel_complex)},
             {r.conjugate, r.count * r.count * sizeof(rfmodel_complex)},
             range});
    });
}

int rfmodel_linearize_highorder_amplifier(const rfmodel_highorder_amplifier_request *request,
                                          const rfmodel_conversion_linearization_output *output) {
    return guarded([&] {
        require(request);
        const auto ranges = highorder_ranges(request->model);
        linearize_conversion_outputs(
            request,
            output,
            [](const auto &r, const auto &channels, const auto &waves) {
                return highorder_point(r.spacing_hz, channels, waves, r.model, r.reference_ohms);
            },
            ranges);
    });
}

int rfmodel_linearize_polynomial_amplifier(const rfmodel_polynomial_amplifier_request *request,
                                           const rfmodel_conversion_linearization_output *output) {
    return guarded([&] {
        require(request);
        const auto range = polynomial_coefficient_range(request->model);
        linearize_conversion_outputs(request,
                                     output,
                                     [](const auto &r, const auto &channels, const auto &waves) {
                                         return rfmodel::linearize_polynomial_amplifier(
                                             r.spacing_hz,
                                             channels,
                                             waves,
                                             polynomial_model(r.model),
                                             r.model.input_port,
                                             r.model.output_port,
                                             r.reference_ohms);
                                     },
                                     {range});
    });
}

static int plan_conversion_frequencies_impl(const rfmodel_frequency_plan_request *request,
                                            const rfmodel_highorder_frequency_rule *highorders,
                                            size_t highorder_count,
                                            const rfmodel_frequency_plan_output *output,
                                            rfmodel_frequency_plan_diagnostics *diagnostics) {
    return guarded([&] {
        require(request && diagnostics);
        require(highorder_count <= 4096 && (highorders || highorder_count == 0));
        const auto &r = *request;
        require(r.devices && r.device_count > 0 && r.device_count <= 512);
        require(r.connection_count <= 256 && (r.connections || r.connection_count == 0));
        using Range = std::pair<const void *, size_t>;
        std::vector<Range> inputs{{request, sizeof(*request)},
                                  {r.devices, r.device_count * sizeof(*r.devices)},
                                  {r.connections, r.connection_count * sizeof(*r.connections)},
                                  {highorders, highorder_count * sizeof(*highorders)}};
        rfmodel::ConversionFrequencyOptions options;
        if (r.options) {
            inputs.emplace_back(r.options, sizeof(*r.options));
            options = {
                r.options->max_total_channels, r.options->max_bin, r.options->max_transitions};
        }
        std::vector<rfmodel::ConversionFrequencyDevice> devices;
        size_t seed_count = 0, port_count = 0, rule_count = highorder_count;
        for (size_t d = 0; d < r.device_count; ++d) {
            const auto &entry = r.devices[d];
            require(entry.ports && entry.port_count > 0 && entry.port_count <= 512 - port_count);
            require(entry.seed_count <= 512 - seed_count && (entry.seeds || entry.seed_count == 0));
            require(entry.rule_count <= 4096 - rule_count &&
                    (entry.rules || entry.rule_count == 0));
            port_count += entry.port_count;
            seed_count += entry.seed_count;
            rule_count += entry.rule_count;
            inputs.emplace_back(entry.ports, entry.port_count * sizeof(size_t));
            inputs.emplace_back(entry.seeds, entry.seed_count * sizeof(*entry.seeds));
            inputs.emplace_back(entry.rules, entry.rule_count * sizeof(*entry.rules));
            rfmodel::ConversionFrequencyDevice device;
            device.ports.assign(entry.ports, entry.ports + entry.port_count);
            for (size_t i = 0; i < entry.seed_count; ++i) {
                device.seeds.push_back({entry.seeds[i].port, entry.seeds[i].bin});
            }
            for (size_t i = 0; i < entry.rule_count; ++i) {
                const auto &rule = entry.rules[i];
                if (rule.kind == RFMODEL_FREQUENCY_POLYNOMIAL) {
                    require(rule.second_input_port == 0 && rule.voltage_coefficients &&
                            rule.coefficient_count > 0 && rule.coefficient_count <= 12);
                    inputs.emplace_back(rule.voltage_coefficients,
                                        rule.coefficient_count * sizeof(double));
                    device.rules.emplace_back(rfmodel::FrequencyPolynomialRule{
                        rule.input_port,
                        rule.output_port,
                        rfmodel::MemorylessPolynomial(std::vector<double>(
                            rule.voltage_coefficients,
                            rule.voltage_coefficients + rule.coefficient_count))});
                } else {
                    require(!rule.voltage_coefficients && rule.coefficient_count == 0);
                    if (rule.kind == RFMODEL_FREQUENCY_COPY) {
                        require(rule.second_input_port == 0);
                        device.rules.emplace_back(
                            rfmodel::FrequencyCopyRule{rule.input_port, rule.output_port});
                    } else {
                        require(rule.kind == RFMODEL_FREQUENCY_MIXER);
                        device.rules.emplace_back(rfmodel::FrequencyMixerRule{
                            rule.input_port, rule.second_input_port, rule.output_port});
                    }
                }
            }
            devices.push_back(std::move(device));
        }
        for (size_t i = 0; i < highorder_count; ++i) {
            const auto &rule = highorders[i];
            require(rule.device < devices.size() && rule.voltage_coefficients &&
                    rule.coefficient_count > 0 && rule.coefficient_count <= 12 &&
                    rule.generating_count <= 512 &&
                    (rule.generating_bins || rule.generating_count == 0));
            inputs.emplace_back(rule.voltage_coefficients, rule.coefficient_count * sizeof(double));
            inputs.emplace_back(rule.generating_bins, rule.generating_count * sizeof(int));
            std::vector<int> generating;
            for (size_t j = 0; j < rule.generating_count; ++j) {
                generating.push_back(rule.generating_bins[j]);
            }
            devices[rule.device].rules.emplace_back(rfmodel::FrequencyHighOrderRule{
                rule.input_port,
                rule.output_port,
                rfmodel::MemorylessPolynomial(std::vector<double>(
                    rule.voltage_coefficients, rule.voltage_coefficients + rule.coefficient_count)),
                std::move(generating)});
        }
        std::vector<rfmodel::ConversionPortConnection> wires;
        for (size_t i = 0; i < r.connection_count; ++i) {
            const auto &wire = r.connections[i];
            wires.push_back(
                {wire.first_device, wire.first_port, wire.second_device, wire.second_port});
        }
        std::vector<Range> outputs{{diagnostics, sizeof(*diagnostics)}};
        if (output) {
            inputs.emplace_back(output, sizeof(*output));
            require(output->channels && output->device_offsets);
            require((output->seed_channel_indices || output->seed_capacity == 0) &&
                    (output->seed_channel_indices || seed_count == 0));
            require(output->channel_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(rfmodel_frequency_channel));
            require(output->device_offset_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(size_t));
            require(output->seed_capacity <= std::numeric_limits<size_t>::max() / sizeof(size_t));
            outputs.emplace_back(output->channels,
                                 output->channel_capacity * sizeof(rfmodel_frequency_channel));
            outputs.emplace_back(output->device_offsets,
                                 output->device_offset_capacity * sizeof(size_t));
            outputs.emplace_back(output->seed_channel_indices,
                                 output->seed_capacity * sizeof(size_t));
        }
        for (size_t i = 0; i < outputs.size(); ++i) {
            for (const auto &input : inputs) {
                disjoint(outputs[i].first, outputs[i].second, input.first, input.second);
            }
            for (size_t j = 0; j < i; ++j) {
                disjoint(outputs[i].first, outputs[i].second, outputs[j].first, outputs[j].second);
            }
        }
        const auto plan =
            rfmodel::plan_conversion_frequencies(r.spacing_hz, devices, wires, options);
        if (output) {
            require(output->channel_capacity >= plan.total_channels &&
                    output->device_offset_capacity >= r.device_count + 1 &&
                    output->seed_capacity >= seed_count);
            size_t channel_index = 0, seed_index = 0;
            for (size_t d = 0; d < r.device_count; ++d) {
                output->device_offsets[d] = channel_index;
                for (auto channel : plan.channels[d]) {
                    output->channels[channel_index++] = {channel.port, channel.bin};
                }
                for (auto local_index : plan.seed_channel_indices[d]) {
                    output->seed_channel_indices[seed_index++] = local_index;
                }
            }
            output->device_offsets[r.device_count] = channel_index;
        }
        *diagnostics = {r.device_count,
                        seed_count,
                        plan.total_channels,
                        plan.generated_channels,
                        plan.iterations,
                        plan.transitions};
    });
}

int rfmodel_plan_conversion_frequencies(const rfmodel_frequency_plan_request *request,
                                        const rfmodel_frequency_plan_output *output,
                                        rfmodel_frequency_plan_diagnostics *diagnostics) {
    return plan_conversion_frequencies_impl(request, nullptr, 0, output, diagnostics);
}

int rfmodel_plan_conversion_frequencies_with_highorders(
    const rfmodel_frequency_plan_request *request,
    const rfmodel_highorder_frequency_rule *highorders,
    size_t highorder_count,
    const rfmodel_frequency_plan_output *output,
    rfmodel_frequency_plan_diagnostics *diagnostics) {
    return plan_conversion_frequencies_impl(
        request, highorders, highorder_count, output, diagnostics);
}

int rfmodel_polynomial_output_bins(const int *input_bins,
                                   size_t input_count,
                                   const double *voltage_coefficients,
                                   size_t coefficient_count,
                                   int *output_bins,
                                   size_t output_capacity,
                                   size_t *output_count) {
    return guarded([&] {
        require(input_bins && input_count > 0 && input_count <= 512 && output_count);
        require((!output_bins && output_capacity == 0) || (output_bins && output_capacity > 0));
        require(output_capacity <= std::numeric_limits<size_t>::max() / sizeof(int));
        const rfmodel_polynomial_amplifier_parameters parameters{
            voltage_coefficients, coefficient_count, 0, 1};
        const auto coefficients = polynomial_coefficient_range(parameters);
        disjoint(output_count, sizeof(size_t), input_bins, input_count * sizeof(int));
        disjoint(output_count, sizeof(size_t), coefficients.first, coefficients.second);
        if (output_bins) {
            const auto bytes = output_capacity * sizeof(int);
            disjoint(output_bins, bytes, output_count, sizeof(size_t));
            disjoint(output_bins, bytes, input_bins, input_count * sizeof(int));
            disjoint(output_bins, bytes, coefficients.first, coefficients.second);
        }
        const auto result = rfmodel::polynomial_output_bins(
            std::vector<int>(input_bins, input_bins + input_count), polynomial_model(parameters));
        if (output_bins) {
            require(output_capacity >= result.size());
            std::copy(result.begin(), result.end(), output_bins);
        }
        *output_count = result.size();
    });
}

int rfmodel_conversion_network_solve_nonlinear(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_nonlinear_model *models,
    size_t model_count,
    const rfmodel_conversion_operating_options *options,
    const rfmodel_conversion_affine_offset *fixed_offset,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded,
    rfmodel_conversion_operating_diagnostics *diagnostics) {
    return guarded([&] {
        require(options && diagnostics);
        analyze_conversion_network_outputs(devices,
                                           device_count,
                                           connections,
                                           connection_count,
                                           output,
                                           loaded,
                                           additional_source_noise,
                                           fixed_offset,
                                           options,
                                           nullptr,
                                           0,
                                           diagnostics,
                                           models,
                                           model_count);
    });
}

int rfmodel_conversion_network_solve_nonlinear_am_pm(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_nonlinear_model *models,
    size_t model_count,
    const rfmodel_conversion_am_pm *phases,
    size_t phase_count,
    const rfmodel_conversion_operating_options *options,
    const rfmodel_conversion_affine_offset *fixed_offset,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded,
    rfmodel_conversion_operating_diagnostics *diagnostics) {
    return guarded([&] {
        require(options && diagnostics);
        analyze_conversion_network_outputs(devices,
                                           device_count,
                                           connections,
                                           connection_count,
                                           output,
                                           loaded,
                                           additional_source_noise,
                                           fixed_offset,
                                           options,
                                           nullptr,
                                           0,
                                           diagnostics,
                                           models,
                                           model_count,
                                           phases,
                                           phase_count);
    });
}

int rfmodel_conversion_network_solve_operating_point(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_bilinear_mixer *mixers,
    size_t mixer_count,
    const rfmodel_conversion_operating_options *options,
    const rfmodel_conversion_affine_offset *fixed_offset,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded,
    rfmodel_conversion_operating_diagnostics *diagnostics) {
    return guarded([&] {
        require(options && diagnostics);
        analyze_conversion_network_outputs(devices,
                                           device_count,
                                           connections,
                                           connection_count,
                                           output,
                                           loaded,
                                           additional_source_noise,
                                           fixed_offset,
                                           options,
                                           mixers,
                                           mixer_count,
                                           diagnostics);
    });
}

int rfmodel_measure_channel_noise(const rfmodel_channel_noise_request *request,
                                  rfmodel_channel_noise_result *output) {
    return guarded([&] {
        require(request && output && request->noise_count >= 2 && request->noise_count <= 1000000 &&
                request->line_count <= 1000000 && request->noise_frequencies_hz &&
                request->noise_densities_w_per_hz &&
                (!request->line_count || (request->line_frequencies_hz && request->line_powers_w)));
        disjoint(output, sizeof(*output), request, sizeof(*request));
        for (auto *input : {request->noise_frequencies_hz, request->noise_densities_w_per_hz}) {
            disjoint(output, sizeof(*output), input, request->noise_count * sizeof(double));
        }
        if (request->line_count) {
            for (auto *input : {request->line_frequencies_hz, request->line_powers_w}) {
                disjoint(output, sizeof(*output), input, request->line_count * sizeof(double));
            }
        }
        std::vector<rfmodel::NoiseDensitySample> noise;
        std::vector<rfmodel::ChannelSignalLine> lines;
        for (size_t i = 0; i < request->noise_count; ++i) {
            noise.push_back(
                {request->noise_frequencies_hz[i], request->noise_densities_w_per_hz[i]});
        }
        for (size_t i = 0; i < request->line_count; ++i) {
            lines.push_back({request->line_frequencies_hz[i], request->line_powers_w[i]});
        }
        const auto result =
            rfmodel::measure_channel_noise(noise, lines, request->center_hz, request->bandwidth_hz);
        *output = {result.lower_frequency_hz,
                   result.upper_frequency_hz,
                   result.effective_bandwidth_hz,
                   result.noise_power_w,
                   result.mean_noise_density_w_per_hz,
                   result.desired_signal_power_w,
                   result.carrier_to_noise_db.value_or(0.),
                   static_cast<int>(result.ratio_state),
                   result.interpolation_intervals,
                   result.desired_line_count};
    });
}

int rfmodel_conversion_analyze(const rfmodel_conversion_request *request,
                               const rfmodel_conversion_output *output) {
    return rfmodel_conversion_network_analyze(request, 1, nullptr, 0, output);
}

int rfmodel_conversion_network_analyze(const rfmodel_conversion_request *devices,
                                       size_t device_count,
                                       const rfmodel_conversion_connection *connections,
                                       size_t connection_count,
                                       const rfmodel_conversion_output *output) {
    return guarded([&] {
        analyze_conversion_network_outputs(
            devices, device_count, connections, connection_count, output, nullptr);
    });
}

int rfmodel_conversion_network_analyze_power_waves(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_references *references,
    const rfmodel_conversion_affine_offset *offset,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded) {
    return guarded([&] {
        require(references);
        analyze_conversion_network_outputs(devices,
                                           device_count,
                                           connections,
                                           connection_count,
                                           output,
                                           loaded,
                                           additional_source_noise,
                                           offset,
                                           nullptr,
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr,
                                           0,
                                           nullptr,
                                           0,
                                           references);
    });
}

int rfmodel_conversion_analyze_power_waves(
    const rfmodel_conversion_request *request,
    const rfmodel_conversion_references *references,
    const rfmodel_conversion_affine_offset *offset,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded) {
    return rfmodel_conversion_network_analyze_power_waves(
        request, 1, nullptr, 0, references, offset, additional_source_noise, output, loaded);
}

int rfmodel_conversion_renormalize_power_waves(
    const rfmodel_conversion_request *request,
    const rfmodel_conversion_references *old_references,
    const rfmodel_conversion_references *new_references,
    const rfmodel_conversion_affine_offset *offset,
    const rfmodel_conversion_renormalization_output *output) {
    return guarded([&] {
        require(output);
        size_t n;
        auto inputs = conversion_input_ranges(request, 1, nullptr, 0, n);
        conversion_reference_ranges(request, 1, old_references, inputs);
        conversion_reference_ranges(request, 1, new_references, inputs);
        inputs.push_back({output, sizeof(*output)});
        if (offset) {
            require(offset->count == n && offset->values);
            inputs.push_back({offset, sizeof(*offset)});
            inputs.push_back({offset->values, n * sizeof(rfmodel_complex)});
        }
        require(output->wave_capacity >= n && output->matrix_capacity >= n * n &&
                output->wave_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex) &&
                output->matrix_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
        std::vector<std::pair<const void *, size_t>> outputs;
        for (auto *matrix : {output->direct,
                             output->conjugate,
                             output->noise_direct,
                             output->noise_conjugate,
                             output->intrinsic_covariance,
                             output->intrinsic_complementary}) {
            require(matrix);
            outputs.push_back({matrix, output->matrix_capacity * sizeof(rfmodel_complex)});
        }
        require(output->offset);
        outputs.push_back({output->offset, output->wave_capacity * sizeof(rfmodel_complex)});
        for (size_t i = 0; i < outputs.size(); ++i) {
            for (const auto &input : inputs) {
                disjoint(outputs[i].first, outputs[i].second, input.first, input.second);
            }
            for (size_t j = 0; j < i; ++j) {
                disjoint(outputs[i].first, outputs[i].second, outputs[j].first, outputs[j].second);
            }
        }
        const auto device = decode_conversion_devices(request, 1, old_references).front();
        const auto change = rfmodel::renormalize_power_waves(
            device.model, decode_conversion_references(*new_references));
        std::vector<rfmodel::Complex> emission(n);
        if (offset) {
            for (size_t i = 0; i < n; ++i) {
                emission[i] = {offset->values[i].real, offset->values[i].imag};
            }
        }
        const auto transformed_offset = rfmodel::renormalize_conversion_offset(change, emission);
        const auto noise = rfmodel::renormalize_conversion_noise(change, device.intrinsic_noise);
        const std::pair<rfmodel_complex *, const rfmodel::SMatrix *> matrices[] = {
            {output->direct, &change.model.direct()},
            {output->conjugate, &change.model.conjugate()},
            {output->noise_direct, &change.noise_direct},
            {output->noise_conjugate, &change.noise_conjugate},
            {output->intrinsic_covariance, &noise.covariance},
            {output->intrinsic_complementary, &noise.complementary}};
        for (const auto &matrix : matrices) {
            for (size_t i = 0; i < n * n; ++i) {
                const auto value = matrix.second->values[i];
                matrix.first[i] = {value.real(), value.imag()};
            }
        }
        for (size_t i = 0; i < n; ++i) {
            output->offset[i] = {transformed_offset[i].real(), transformed_offset[i].imag()};
        }
    });
}

int rfmodel_conversion_renormalize_boundary_power_waves(
    const rfmodel_conversion_request *request,
    const rfmodel_conversion_references *old_references,
    const rfmodel_conversion_references *new_references,
    const size_t *external_channels,
    size_t external_count,
    const rfmodel_conversion_boundary_output *output) {
    return guarded([&] {
        require(output);
        size_t n;
        auto inputs = conversion_input_ranges(request, 1, nullptr, 0, n);
        conversion_reference_ranges(request, 1, old_references, inputs);
        conversion_reference_ranges(request, 1, new_references, inputs);
        require(external_count <= n && (!external_count || external_channels));
        inputs.push_back({external_channels, external_count * sizeof(size_t)});
        inputs.push_back({output, sizeof(*output)});
        require(output->wave_capacity >= n && output->matrix_capacity >= n * n &&
                output->wave_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex) &&
                output->matrix_capacity <=
                    std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
        std::vector<std::pair<const void *, size_t>> outputs;
        for (auto *vector : {output->source, output->reflection}) {
            require(vector);
            outputs.push_back({vector, output->wave_capacity * sizeof(rfmodel_complex)});
        }
        for (auto *matrix : {output->source_covariance, output->source_complementary}) {
            require(matrix);
            outputs.push_back({matrix, output->matrix_capacity * sizeof(rfmodel_complex)});
        }
        for (size_t i = 0; i < outputs.size(); ++i) {
            for (const auto &input : inputs) {
                disjoint(outputs[i].first, outputs[i].second, input.first, input.second);
            }
            for (size_t j = 0; j < i; ++j) {
                disjoint(outputs[i].first, outputs[i].second, outputs[j].first, outputs[j].second);
            }
        }
        const auto device = decode_conversion_devices(request, 1, old_references).front();
        std::vector<size_t> external;
        if (external_count) {
            external.assign(external_channels, external_channels + external_count);
        }
        const auto boundary =
            rfmodel::renormalize_conversion_boundary(device.model,
                                                     decode_conversion_references(*new_references),
                                                     external,
                                                     device.source,
                                                     device.reflection,
                                                     device.source_noise);
        for (size_t i = 0; i < n; ++i) {
            output->source[i] = {boundary.source[i].real(), boundary.source[i].imag()};
            output->reflection[i] = {boundary.reflection[i].real(), boundary.reflection[i].imag()};
        }
        for (size_t i = 0; i < n * n; ++i) {
            const auto c = boundary.source_noise.covariance.values[i];
            const auto p = boundary.source_noise.complementary.values[i];
            output->source_covariance[i] = {c.real(), c.imag()};
            output->source_complementary[i] = {p.real(), p.imag()};
        }
    });
}

int rfmodel_conversion_network_analyze_affine(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_affine_offset *offset,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded) {
    return guarded([&] {
        require(offset);
        analyze_conversion_network_outputs(devices,
                                           device_count,
                                           connections,
                                           connection_count,
                                           output,
                                           loaded,
                                           additional_source_noise,
                                           offset);
    });
}

int rfmodel_conversion_network_analyze_correlated(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_source_noise *additional_source_noise,
    const rfmodel_conversion_output *output,
    const rfmodel_conversion_loaded_output *loaded) {
    return guarded([&] {
        require(additional_source_noise);
        analyze_conversion_network_outputs(devices,
                                           device_count,
                                           connections,
                                           connection_count,
                                           output,
                                           loaded,
                                           additional_source_noise);
    });
}

int rfmodel_conversion_network_analyze_loaded(const rfmodel_conversion_request *devices,
                                              size_t device_count,
                                              const rfmodel_conversion_connection *connections,
                                              size_t connection_count,
                                              const rfmodel_conversion_output *output,
                                              const rfmodel_conversion_loaded_output *loaded) {
    return guarded([&] {
        require(loaded);
        analyze_conversion_network_outputs(
            devices, device_count, connections, connection_count, output, loaded);
    });
}

static int conversion_network_noise_analysis_impl(const rfmodel_conversion_request *devices,
                                                  size_t device_count,
                                                  const rfmodel_conversion_connection *connections,
                                                  size_t connection_count,
                                                  const rfmodel_conversion_references *references,
                                                  const rfmodel_conversion_noise_request *request,
                                                  rfmodel_conversion_noise_result *output,
                                                  bool explicit_references) {
    return guarded([&] {
        require(!explicit_references || references);
        require(request && output && request->reference_channels && request->thermal_channels &&
                request->reference_count > 0 && request->reference_count <= 512 &&
                request->thermal_count > 0 && request->thermal_count <= 512);
        size_t total;
        auto inputs =
            conversion_input_ranges(devices, device_count, connections, connection_count, total);
        if (references) {
            conversion_reference_ranges(devices, device_count, references, inputs);
        }
        inputs.push_back({request, sizeof(*request)});
        inputs.push_back({request->reference_channels, request->reference_count * sizeof(size_t)});
        inputs.push_back({request->thermal_channels, request->thermal_count * sizeof(size_t)});
        for (const auto &input : inputs) {
            if (input.second) {
                disjoint(output, sizeof(*output), input.first, input.second);
            }
        }
        const auto network = decode_conversion_network(
            devices, device_count, connections, connection_count, references);
        const auto result = network.reference_noise_analysis(
            {request->reference_channels, request->reference_channels + request->reference_count},
            {request->thermal_channels, request->thermal_channels + request->thermal_count},
            request->output_channel,
            request->reference_temperature_k);
        *output = {result.reference_gain,
                   result.reference_output_noise_w_per_hz,
                   result.output_noise_w_per_hz,
                   result.noise_factor,
                   result.noise_figure_db,
                   result.equivalent_input_temperature_k};
    });
}

int rfmodel_conversion_network_noise_analysis(const rfmodel_conversion_request *devices,
                                              size_t device_count,
                                              const rfmodel_conversion_connection *connections,
                                              size_t connection_count,
                                              const rfmodel_conversion_noise_request *request,
                                              rfmodel_conversion_noise_result *output) {
    return conversion_network_noise_analysis_impl(
        devices, device_count, connections, connection_count, nullptr, request, output, false);
}

int rfmodel_conversion_network_noise_analysis_power_waves(
    const rfmodel_conversion_request *devices,
    size_t device_count,
    const rfmodel_conversion_connection *connections,
    size_t connection_count,
    const rfmodel_conversion_references *references,
    const rfmodel_conversion_noise_request *request,
    rfmodel_conversion_noise_result *output) {
    return conversion_network_noise_analysis_impl(
        devices, device_count, connections, connection_count, references, request, output, true);
}

int rfmodel_ideal_mixer_conversion(double spacing_hz,
                                   const size_t *physical_ports,
                                   const int *bins,
                                   size_t count,
                                   int lo_bin,
                                   double gain_db,
                                   double phase_radians,
                                   size_t rf_port,
                                   size_t if_port,
                                   double reference_ohms,
                                   rfmodel_complex *direct,
                                   rfmodel_complex *conjugate,
                                   size_t matrix_capacity) {
    return guarded([&] {
        require(count > 0 && count <= 512 && physical_ports && bins && direct && conjugate &&
                matrix_capacity >= count * count &&
                matrix_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
        const auto bytes = matrix_capacity * sizeof(rfmodel_complex);
        disjoint(direct, bytes, conjugate, bytes);
        for (auto *output : {direct, conjugate}) {
            disjoint(output, bytes, physical_ports, count * sizeof(size_t));
            disjoint(output, bytes, bins, count * sizeof(int));
        }
        std::vector<rfmodel::ConversionChannel> channels;
        for (size_t i = 0; i < count; ++i) {
            channels.push_back({physical_ports[i], bins[i]});
        }
        const auto model = rfmodel::ideal_mixer_conversion(
            spacing_hz, channels, lo_bin, gain_db, phase_radians, rf_port, if_port, reference_ohms);
        for (size_t i = 0; i < count * count; ++i) {
            const auto a = model.direct().values[i], b = model.conjugate().values[i];
            direct[i] = {a.real(), a.imag()};
            conjugate[i] = {b.real(), b.imag()};
        }
    });
}

int rfmodel_butterworth_s(double frequency_hz,
                          const rfmodel_butterworth_parameters *parameters,
                          rfmodel_complex *output,
                          size_t capacity) {
    return guarded([&] {
        require(parameters && output && capacity >= 4 &&
                capacity <= std::numeric_limits<size_t>::max() / sizeof(*output));
        require(parameters->input_stopband_open == 0 || parameters->input_stopband_open == 1);
        disjoint(parameters, sizeof(*parameters), output, capacity * sizeof(*output));
        rfmodel::ButterworthFilterParameters decoded;
        decoded.response = static_cast<rfmodel::ButterworthResponse>(parameters->response);
        decoded.order = parameters->order;
        decoded.lower_passband_hz = parameters->lower_passband_hz;
        decoded.upper_passband_hz = parameters->upper_passband_hz;
        decoded.passband_attenuation_db = parameters->passband_attenuation_db;
        decoded.input_stopband_open = parameters->input_stopband_open == 1;
        decoded.reference_ohms = parameters->reference_ohms;
        const rfmodel::ButterworthFilterModel model("butterworth_ladder", decoded);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_chebyshev_s(double frequency_hz,
                        const rfmodel_chebyshev_parameters *parameters,
                        rfmodel_complex *output,
                        size_t capacity) {
    return guarded([&] {
        require(parameters && output && capacity >= 4 &&
                capacity <= std::numeric_limits<size_t>::max() / sizeof(*output));
        require(parameters->input_stopband_open == 0 || parameters->input_stopband_open == 1);
        disjoint(parameters, sizeof(*parameters), output, capacity * sizeof(*output));
        rfmodel::ChebyshevFilterParameters decoded;
        decoded.response = static_cast<rfmodel::FilterResponse>(parameters->response);
        decoded.order = parameters->order;
        decoded.lower_passband_hz = parameters->lower_passband_hz;
        decoded.upper_passband_hz = parameters->upper_passband_hz;
        decoded.ripple_db = parameters->ripple_db;
        decoded.passband_attenuation_db = parameters->passband_attenuation_db;
        decoded.input_stopband_open = parameters->input_stopband_open == 1;
        decoded.reference_ohms = parameters->reference_ohms;
        const rfmodel::ChebyshevFilterModel model("chebyshev_lossless", decoded);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_ideal_rlc_s(double frequency_hz,
                        rfmodel_ideal_element element,
                        rfmodel_lumped_connection connection,
                        double value,
                        double reference_ohms,
                        rfmodel_complex *output,
                        size_t capacity) {
    return guarded([&] {
        const rfmodel::IdealRLCModel model("ideal_rlc",
                                           static_cast<rfmodel::IdealElement>(element),
                                           static_cast<rfmodel::LumpedConnection>(connection),
                                           value,
                                           reference_ohms);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_matched_transmission_s(double frequency_hz,
                                   double loss_db,
                                   double delay_s,
                                   double reference_ohms,
                                   rfmodel_complex *output,
                                   size_t capacity) {
    return guarded([&] {
        const rfmodel::MatchedTransmissionModel model(
            "matched_transmission", loss_db, delay_s, reference_ohms);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_equal_power_divider_s(double frequency_hz,
                                  size_t branches,
                                  double excess_loss_db,
                                  double reference_ohms,
                                  rfmodel_complex *output,
                                  size_t capacity) {
    return guarded([&] {
        const rfmodel::EqualPowerDividerModel model(
            "equal_power_divider", branches, excess_loss_db, reference_ohms);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_isolated_power_divider_s(double frequency_hz,
                                     const rfmodel_complex *branch_transmissions,
                                     size_t branches,
                                     double reference_ohms,
                                     rfmodel_complex *output,
                                     size_t capacity) {
    return guarded([&] {
        require(branch_transmissions && output && branches >= 2 && branches <= 64);
        require(capacity <= std::numeric_limits<size_t>::max() / sizeof(*output));
        disjoint(branch_transmissions,
                 branches * sizeof(*branch_transmissions),
                 output,
                 capacity * sizeof(*output));
        std::vector<rfmodel::Complex> gains;
        for (size_t i = 0; i < branches; ++i) {
            gains.push_back({branch_transmissions[i].real, branch_transmissions[i].imag});
        }
        const rfmodel::IsolatedPowerDividerModel model(
            "isolated_power_divider", gains, reference_ohms);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_quadrature_coupler_s(double frequency_hz,
                                 double coupled_power_fraction,
                                 double excess_loss_db,
                                 double reference_ohms,
                                 rfmodel_complex *output,
                                 size_t capacity) {
    return guarded([&] {
        const rfmodel::QuadratureCouplerModel model(
            "quadrature_coupler", coupled_power_fraction, excess_loss_db, reference_ohms);
        write_passive_s(model.s_parameters(frequency_hz), output, capacity);
    });
}

int rfmodel_power_wave_noise_figure(const rfmodel_complex *scattering,
                                    size_t value_count,
                                    const rfmodel_complex *intrinsic_noise,
                                    const rfmodel_complex *references,
                                    rfmodel_complex source_impedance_ohms,
                                    double temperature_k,
                                    double *output_db) {
    return guarded([&] {
        const auto matrix = decode_noise_two_port(scattering, value_count);
        const rfmodel::NoiseCorrelation noise{decode_noise_two_port(intrinsic_noise, value_count)};
        const auto refs = decode_noise_references(references);
        noise_output_disjoint(
            scattering, intrinsic_noise, references, output_db, sizeof(*output_db));
        const double result = rfmodel::power_wave_noise_figure_db(
            matrix,
            noise,
            refs,
            {source_impedance_ohms.real, source_impedance_ohms.imag},
            temperature_k);
        *output_db = result;
    });
}

int rfmodel_power_wave_extract_noise_parameters(const rfmodel_complex *scattering,
                                                size_t value_count,
                                                const rfmodel_complex *intrinsic_noise,
                                                const rfmodel_complex *references,
                                                double temperature_k,
                                                rfmodel_noise_parameters *output) {
    return guarded([&] {
        const auto matrix = decode_noise_two_port(scattering, value_count);
        const rfmodel::NoiseCorrelation noise{decode_noise_two_port(intrinsic_noise, value_count)};
        const auto refs = decode_noise_references(references);
        noise_output_disjoint(scattering, intrinsic_noise, references, output, sizeof(*output));
        const auto result =
            rfmodel::extract_power_wave_noise_parameters(matrix, noise, refs, temperature_k);
        *output = {
            result.minimum_noise_figure_db,
            {result.optimum_source_reflection.real(), result.optimum_source_reflection.imag()},
            result.noise_resistance_ohms};
    });
}

int rfmodel_power_wave_noise_from_parameters(const rfmodel_complex *scattering,
                                             size_t value_count,
                                             const rfmodel_noise_parameters *parameters,
                                             const rfmodel_complex *references,
                                             double temperature_k,
                                             rfmodel_complex *output,
                                             size_t capacity) {
    return guarded([&] {
        require(parameters && capacity >= 4 &&
                capacity <= std::numeric_limits<size_t>::max() / sizeof(*output));
        const auto matrix = decode_noise_two_port(scattering, value_count);
        const auto refs = decode_noise_references(references);
        noise_output_disjoint(scattering, nullptr, references, output, capacity * sizeof(*output));
        disjoint(parameters, sizeof(*parameters), output, capacity * sizeof(*output));
        const rfmodel::TwoPortNoiseParameters decoded{parameters->minimum_noise_figure_db,
                                                      {parameters->optimum_source_reflection.real,
                                                       parameters->optimum_source_reflection.imag},
                                                      parameters->noise_resistance_ohms};
        const auto result =
            rfmodel::noise_from_power_wave_parameters(matrix, decoded, refs, temperature_k);
        for (size_t i = 0; i < 4; ++i) {
            output[i] = {result.watts_per_hz.values[i].real(),
                         result.watts_per_hz.values[i].imag()};
        }
    });
}

int rfmodel_power_wave_renormalize(size_t ports,
                                   const rfmodel_complex *scattering,
                                   size_t value_count,
                                   const rfmodel_complex *old_references,
                                   const rfmodel_complex *new_references,
                                   const rfmodel_complex *intrinsic_noise,
                                   rfmodel_complex *new_scattering,
                                   rfmodel_complex *new_noise,
                                   size_t capacity) {
    return guarded([&] {
        require(ports >= 1 && ports <= 1024 && value_count == ports * ports);
        require(scattering && old_references && new_references && new_scattering);
        require((intrinsic_noise == nullptr) == (new_noise == nullptr) && capacity >= value_count);
        require(capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
        const std::array<const void *, 6> pointers{
            scattering, old_references, new_references, intrinsic_noise, new_scattering, new_noise};
        const size_t matrix_bytes = value_count * sizeof(rfmodel_complex);
        const size_t output_bytes = capacity * sizeof(rfmodel_complex);
        const std::array<size_t, 6> sizes{matrix_bytes,
                                          ports * sizeof(rfmodel_complex),
                                          ports * sizeof(rfmodel_complex),
                                          intrinsic_noise ? matrix_bytes : 0,
                                          output_bytes,
                                          new_noise ? output_bytes : 0};
        for (size_t i = 4; i < pointers.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                disjoint(pointers[i], sizes[i], pointers[j], sizes[j]);
            }
        }
        rfmodel::SMatrix matrix{ports, std::vector<rfmodel::Complex>(value_count)}, noise = matrix;
        std::vector<rfmodel::Complex> old_values, new_values;
        for (size_t i = 0; i < ports; ++i) {
            old_values.push_back({old_references[i].real, old_references[i].imag});
            new_values.push_back({new_references[i].real, new_references[i].imag});
        }
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {scattering[i].real, scattering[i].imag};
            if (intrinsic_noise) {
                noise.values[i] = {intrinsic_noise[i].real, intrinsic_noise[i].imag};
            }
        }
        const auto result = rfmodel::renormalize_power_waves(matrix, old_values, new_values);
        if (intrinsic_noise) {
            noise = rfmodel::propagate_noise(result.noise_transfer, {noise}).watts_per_hz;
        }
        for (size_t i = 0; i < value_count; ++i) {
            new_scattering[i] = {result.scattering.values[i].real(),
                                 result.scattering.values[i].imag()};
            if (new_noise) {
                new_noise[i] = {noise.values[i].real(), noise.values[i].imag()};
            }
        }
    });
}

int rfmodel_power_wave_s_to_parameters(size_t ports,
                                       const rfmodel_complex *scattering,
                                       size_t value_count,
                                       const rfmodel_complex *references,
                                       int admittance,
                                       rfmodel_complex *output,
                                       size_t capacity) {
    return guarded([&] {
        require(ports >= 1 && ports <= 1024 && value_count == ports * ports);
        require(scattering && references && output && capacity >= value_count);
        require(admittance == 0 || admittance == 1);
        require(capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
        disjoint(scattering, value_count * sizeof(*scattering), output, capacity * sizeof(*output));
        disjoint(references, ports * sizeof(*references), output, capacity * sizeof(*output));
        rfmodel::SMatrix matrix{ports, std::vector<rfmodel::Complex>(value_count)};
        std::vector<rfmodel::Complex> values;
        for (size_t i = 0; i < ports; ++i) {
            values.push_back({references[i].real, references[i].imag});
        }
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {scattering[i].real, scattering[i].imag};
        }
        const auto result =
            admittance ? rfmodel::s_to_y(matrix, values) : rfmodel::s_to_z(matrix, values);
        for (size_t i = 0; i < value_count; ++i) {
            output[i] = {result.values[i].real(), result.values[i].imag()};
        }
    });
}

int rfmodel_passive_noise(size_t ports,
                          const rfmodel_complex *scattering,
                          size_t value_count,
                          double temperature_k,
                          rfmodel_complex *covariance,
                          size_t capacity) {
    return guarded([&] {
        require(ports > 0 && ports <= 1024 && scattering && covariance);
        require(value_count == ports * ports && capacity >= value_count);
        rfmodel::SMatrix matrix{ports, std::vector<rfmodel::Complex>(value_count)};
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {scattering[i].real, scattering[i].imag};
        }
        const auto result = rfmodel::passive_thermal_noise(matrix, temperature_k).watts_per_hz;
        for (size_t i = 0; i < value_count; ++i) {
            covariance[i] = {result.values[i].real(), result.values[i].imag()};
        }
    });
}

int rfmodel_network_external_noise(const rfmodel_network *network,
                                   const size_t *ports,
                                   size_t port_count,
                                   const rfmodel_complex *intrinsic,
                                   size_t value_count,
                                   rfmodel_complex *covariance,
                                   size_t capacity) {
    return guarded([&] {
        require(network && ports && intrinsic && covariance && port_count > 0 &&
                port_count <= 1024);
        require(network->ports > 0 && value_count == network->ports * network->ports);
        require(capacity >= port_count * port_count);
        rfmodel::SMatrix matrix{network->ports, std::vector<rfmodel::Complex>(value_count)};
        for (size_t i = 0; i < value_count; ++i) {
            matrix.values[i] = {intrinsic[i].real, intrinsic[i].imag};
        }
        const auto result =
            network->core.external_noise(std::vector<size_t>(ports, ports + port_count), {matrix})
                .watts_per_hz;
        for (size_t i = 0; i < result.values.size(); ++i) {
            covariance[i] = {result.values[i].real(), result.values[i].imag()};
        }
    });
}

int rfmodel_loaded_noise(size_t ports,
                         const rfmodel_complex *scattering,
                         const rfmodel_complex *intrinsic,
                         const rfmodel_complex *boundary_emission,
                         size_t value_count,
                         const rfmodel_complex *reflections,
                         size_t reflection_count,
                         rfmodel_complex *incident,
                         rfmodel_complex *outgoing,
                         size_t matrix_capacity,
                         double *net_into_device,
                         size_t power_capacity) {
    return guarded([&] {
        require(ports > 0 && ports <= 1024);
        require(scattering && intrinsic && boundary_emission && reflections && incident &&
                outgoing && net_into_device);
        require(value_count == ports * ports && reflection_count == ports &&
                matrix_capacity >= value_count && power_capacity >= ports);
        rfmodel::SMatrix s{ports, std::vector<rfmodel::Complex>(value_count)}, c = s, e = s;
        std::vector<rfmodel::Complex> gamma(ports);
        for (size_t i = 0; i < value_count; ++i) {
            s.values[i] = {scattering[i].real, scattering[i].imag};
            c.values[i] = {intrinsic[i].real, intrinsic[i].imag};
            e.values[i] = {boundary_emission[i].real, boundary_emission[i].imag};
        }
        for (size_t i = 0; i < ports; ++i) {
            gamma[i] = {reflections[i].real, reflections[i].imag};
        }
        const auto result = rfmodel::loaded_noise(s, {c}, gamma, {e});
        for (size_t i = 0; i < value_count; ++i) {
            const auto a = result.incident.watts_per_hz.values[i];
            const auto b = result.outgoing.watts_per_hz.values[i];
            incident[i] = {a.real(), a.imag()};
            outgoing[i] = {b.real(), b.imag()};
        }
        for (size_t i = 0; i < ports; ++i) {
            net_into_device[i] = result.net_into_device_w_per_hz[i];
        }
    });
}

int rfmodel_thermal_boundary_noise(size_t ports,
                                   const rfmodel_complex *reflections,
                                   const double *temperatures_k,
                                   rfmodel_complex *covariance,
                                   size_t capacity) {
    return guarded([&] {
        require(ports > 0 && ports <= 1024 && reflections && temperatures_k && covariance);
        require(capacity >= ports * ports);
        std::vector<rfmodel::Complex> gamma(ports);
        for (size_t i = 0; i < ports; ++i) {
            gamma[i] = {reflections[i].real, reflections[i].imag};
        }
        const auto result = rfmodel::thermal_boundary_noise(
                                gamma, std::vector<double>(temperatures_k, temperatures_k + ports))
                                .watts_per_hz;
        for (size_t i = 0; i < result.values.size(); ++i) {
            covariance[i] = {result.values[i].real(), result.values[i].imag()};
        }
    });
}
} // extern "C"
