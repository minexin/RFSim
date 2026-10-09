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
        require(output_count && coefficients && coefficient_count > 0 && coefficient_count <= 10);
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
