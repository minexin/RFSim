#include "rfmodel/amplifier_linearization.hpp"
#include "rfmodel/conversion_operating_point.hpp"
#include "rfmodel/channel_noise.hpp"
#include "rfmodel/phase_noise.hpp"
#include "rfmodel/mixer_linearization.hpp"
#include "rfmodel/conversion_network.hpp"
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
#include "rfmodel/loaded_noise.hpp"
#include "rfmodel/amplifier_model.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
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

std::vector<rfmodel::ConversionDevice>
decode_conversion_devices(const rfmodel_conversion_request *devices, size_t device_count) {
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
        rfmodel::FrequencyConversionModel model(
            r.spacing_hz, channels, matrix(r.direct), matrix(r.conjugate), r.reference_ohms);
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
                          size_t connection_count) {
    rfmodel::FrequencyConversionNetwork network(devices[0].spacing_hz, devices[0].reference_ohms);
    for (auto device : decode_conversion_devices(devices, device_count)) {
        network.add(std::move(device));
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
    size_t model_count = 0) {
    require(output);
    const auto &o = *output;
    size_t total;
    auto inputs =
        conversion_input_ranges(devices, device_count, connections, connection_count, total);
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
        decode_conversion_network(devices, device_count, connections, connection_count);
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
void linearize_conversion_outputs(const Request *request,
                                  const rfmodel_mixer_linearization_output *output,
                                  Evaluate evaluate) {
    require(request && output);
    const auto &r = *request;
    const auto &o = *output;
    require(r.count && r.count <= 512 && r.physical_ports && r.bins && r.operating_incident &&
            o.direct && o.conjugate && o.operating_outgoing &&
            o.matrix_capacity >= r.count * r.count && o.wave_capacity >= r.count &&
            o.matrix_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex) &&
            o.wave_capacity <= std::numeric_limits<size_t>::max() / sizeof(rfmodel_complex));
    const std::vector<std::pair<const void *, size_t>> inputs{
        {request, sizeof(*request)},
        {output, sizeof(*output)},
        {r.physical_ports, r.count * sizeof(size_t)},
        {r.bins, r.count * sizeof(int)},
        {r.operating_incident, r.count * sizeof(rfmodel_complex)}};
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

extern "C" {
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
        *out = new rfmodel_touchstone(rfmodel::read_touchstone(path_utf8), policy);
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

int rfmodel_conversion_network_noise_analysis(const rfmodel_conversion_request *devices,
                                              size_t device_count,
                                              const rfmodel_conversion_connection *connections,
                                              size_t connection_count,
                                              const rfmodel_conversion_noise_request *request,
                                              rfmodel_conversion_noise_result *output) {
    return guarded([&] {
        require(request && output && request->reference_channels && request->thermal_channels &&
                request->reference_count > 0 && request->reference_count <= 512 &&
                request->thermal_count > 0 && request->thermal_count <= 512);
        size_t total;
        auto inputs =
            conversion_input_ranges(devices, device_count, connections, connection_count, total);
        inputs.push_back({request, sizeof(*request)});
        inputs.push_back({request->reference_channels, request->reference_count * sizeof(size_t)});
        inputs.push_back({request->thermal_channels, request->thermal_count * sizeof(size_t)});
        for (const auto &input : inputs) {
            if (input.second) {
                disjoint(output, sizeof(*output), input.first, input.second);
            }
        }
        const auto network =
            decode_conversion_network(devices, device_count, connections, connection_count);
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
