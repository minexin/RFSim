#pragma once
#include "rfmodel/adapters/model_adapter.hpp"
#include "rfmodel/conversion/amplifier_linearization.hpp"
#include "rfmodel/conversion/mixer_linearization.hpp"
#include "rfmodel/conversion/highorder_linearization.hpp"
#include "rfmodel/conversion/polynomial_linearization.hpp"
#include "rfmodel/conversion/tabulated_amplifier_linearization.hpp"
#include "rfmodel/nonlinear/polynomial_amplifier.hpp"
#include <type_traits>

namespace rfmodel {
namespace model_adapter_detail {
template <class Model> ModelDescriptor descriptor(const Model &model, ModelIdentity identity) {
    static_assert(std::is_base_of<RFDeviceModel, Model>::value,
                  "provider adapters require RFDeviceModel metadata");
    if (model.port_count() == 0 || model.port_count() > 1024) {
        throw std::invalid_argument("invalid provider port count");
    }
    ModelDescriptor result{std::move(identity), model.name(), {}};
    for (std::size_t i = 0; i < model.port_count(); ++i) {
        result.ports.push_back(model.port(i));
    }
    return result;
}

template <class Model> ModelFunctions providers(std::shared_ptr<const Model> model) {
    ModelFunctions result;
    if constexpr (std::is_base_of<SParameterProvider, Model>::value) {
        result.scattering = [model](double f) {
            return model->s_parameters(f);
        };
    }
    if constexpr (std::is_base_of<NoiseCorrelationProvider, Model>::value) {
        result.noise = [model](double f) {
            return model->noise_correlation(f);
        };
    }
    if constexpr (std::is_base_of<SpectrumTransmissionProvider, Model>::value) {
        result.spectrum = [model](const auto &input) {
            return model->transmit(input);
        };
    }
    return result;
}
} // namespace model_adapter_detail

// Copies/moves the concrete model into an immutable owned snapshot. No RTTI,
// solver registration, implicit thermal noise, or vendor parameter conversion.
template <class Model> ModelAdapter adapt_device_model(Model model, ModelIdentity identity) {
    auto descriptor = model_adapter_detail::descriptor(model, std::move(identity));
    auto owned = std::make_shared<const Model>(std::move(model));
    return {std::move(descriptor), model_adapter_detail::providers(std::move(owned))};
}

// Explicit opt-in temperature. Passivity is checked at each evaluated frequency.
template <class Model>
ModelAdapter adapt_passive_model(Model model, ModelIdentity identity, double temperature_k) {
    static_assert(std::is_base_of<SParameterProvider, Model>::value,
                  "passive adapter requires scattering parameters");
    if (!std::isfinite(temperature_k) || temperature_k < 0.) {
        throw std::invalid_argument("invalid passive adapter temperature");
    }
    auto descriptor = model_adapter_detail::descriptor(model, std::move(identity));
    auto owned = std::make_shared<const Model>(std::move(model));
    auto functions = model_adapter_detail::providers(owned);
    functions.noise = [owned, temperature_k](double f) {
        return passive_thermal_noise(owned->s_parameters(f), temperature_k);
    };
    return {std::move(descriptor), std::move(functions)};
}

inline ModelAdapter adapt_polynomial_amplifier(MatchedPolynomialAmplifier model,
                                               ModelIdentity identity,
                                               std::size_t max_source_order = 11) {
    if (max_source_order == 0 || max_source_order > 256) {
        throw std::invalid_argument("invalid adapter source order");
    }
    auto descriptor = model_adapter_detail::descriptor(model, std::move(identity));
    const double reference = descriptor.ports[0].reference_impedance.real();
    std::vector<double> coefficients;
    for (std::size_t n = 0; n <= maximum_polynomial_order; ++n) {
        coefficients.push_back(model.voltage_coefficient(n));
    }
    auto owned = std::make_shared<const MatchedPolynomialAmplifier>(std::move(model));
    auto functions = model_adapter_detail::providers(owned);
    functions.operating_point = [polynomial = MemorylessPolynomial(coefficients),
                                 reference](const ModelOperatingPoint &point) {
        return linearize_polynomial_amplifier(
            point.spacing_hz, point.channels, point.incident, polynomial, 0, 1, reference);
    };
    if (coefficients[0] == 0.) {
        functions.coherent = [coefficients = std::move(coefficients),
                              max_source_order](auto &pipeline, const auto &inputs) {
            auto result = pipeline.polynomial(inputs.at(0), coefficients, max_source_order);
            CoherentSystem::StageStatistics stats;
            stats.polynomial = std::move(result.counts);
            return CoherentSystem::StageEvaluation{{result.output}, std::move(stats)};
        };
    }
    return {std::move(descriptor), std::move(functions)};
}

// Two independent views of the same immutable model: resolved-wave local
// derivatives use generating_bins; source-history propagation uses mode/cutoff.
// Empty generating_bins explicitly disables local product generation only.
inline ModelAdapter
adapt_highorder_amplifier(std::string name,
                          CoherentHighOrderAmplifier model,
                          std::vector<int> generating_bins,
                          std::size_t max_source_order = 11,
                          AmplifierInputMode mode = AmplifierInputMode::sources_only) {
    if (max_source_order == 0 || max_source_order > 256 ||
        (mode != AmplifierInputMode::sources_only &&
         mode != AmplifierInputMode::propagate_distortion)) {
        throw std::invalid_argument("invalid high-order adapter source policy");
    }
    std::set<int> unique;
    if (generating_bins.size() > 512) {
        throw std::invalid_argument("too many high-order adapter generating bins");
    }
    for (auto bin : generating_bins) {
        if (bin <= 0 || !unique.insert(bin).second) {
            throw std::invalid_argument("invalid high-order adapter generating bin");
        }
    }
    const double reference = model.reference_ohms();
    ModelDescriptor descriptor{{"rfmodel.coherent_highorder_amplifier", "1", "", ""},
                               std::move(name),
                               {{0, "in", reference}, {1, "out", reference}}};
    auto owned = std::make_shared<const CoherentHighOrderAmplifier>(std::move(model));
    ModelFunctions functions;
    functions.operating_point =
        [owned, generating_bins = std::move(generating_bins)](const ModelOperatingPoint &point) {
            return linearize_highorder_amplifier(
                point.spacing_hz, point.channels, point.incident, *owned, generating_bins);
        };
    functions.coherent = [owned, max_source_order, mode](auto &pipeline, const auto &inputs) {
        auto result = pipeline.highorder_amplifier(inputs.at(0), *owned, max_source_order, mode);
        return CoherentSystem::StageEvaluation{
            {result.output},
            CoherentSystem::StageStatistics{result.input_power_w,
                                            {},
                                            result.operating_point,
                                            std::move(result.nonlinear_counts),
                                            result.direct_contribution_count,
                                            result.discarded_direct_contributions}};
    };
    return {std::move(descriptor), std::move(functions)};
}

// Three physical ports, with the actual RF and LO waves supplied by the network.
// The fixed normalization never follows the current pump magnitude.
inline ModelAdapter adapt_bilinear_mixer(std::string name,
                                         double gain_db,
                                         double lo_reference_amplitude,
                                         double reference_ohms = 50.) {
    // Reuse the native parameter checks on a closed, zero DC operating point.
    // This rejects unrepresentable normalization at construction, before use.
    linearize_bilinear_mixer(1.,
                             {{0, 0}, {1, 0}, {2, 0}},
                             {0., 0., 0.},
                             gain_db,
                             lo_reference_amplitude,
                             0,
                             1,
                             2,
                             reference_ohms);
    ModelDescriptor descriptor{
        {"rfmodel.bilinear_mixer", "1", "", ""},
        std::move(name),
        {{0, "RF", reference_ohms}, {1, "LO", reference_ohms}, {2, "IF", reference_ohms}}};
    ModelFunctions functions;
    functions.operating_point =
        [gain_db, lo_reference_amplitude, reference_ohms](const ModelOperatingPoint &point) {
            auto result = linearize_bilinear_mixer(point.spacing_hz,
                                                   point.channels,
                                                   point.incident,
                                                   gain_db,
                                                   lo_reference_amplitude,
                                                   0,
                                                   1,
                                                   2,
                                                   reference_ohms);
            // The solver needs F(a), not the bilinear affine offset -F(a).
            return ConversionLinearization{std::move(result.incremental_model),
                                           std::move(result.operating_outgoing)};
        };
    return {std::move(descriptor), std::move(functions)};
}

inline ModelAdapter adapt_saturating_amplifier(std::string name,
                                               SaturatingFundamentalCompression model,
                                               double reference_ohms = 50.) {
    ModelDescriptor descriptor{{"rfmodel.saturating_fundamental", "1", "", ""},
                               std::move(name),
                               {{0, "in", reference_ohms}, {1, "out", reference_ohms}}};
    ModelFunctions functions;
    functions.operating_point = [model, reference_ohms](const ModelOperatingPoint &point) {
        return linearize_saturating_amplifier(
            point.spacing_hz, point.channels, point.incident, model, 0, 1, {}, reference_ohms);
    };
    functions.coherent = [model](auto &pipeline, const auto &inputs) {
        const auto result = pipeline.compress_fundamentals(inputs.at(0), model);
        CoherentSystem::StageStatistics stats;
        stats.input_power_w = result.input_power_w;
        stats.common_amplitude_gain = result.common_amplitude_gain;
        return CoherentSystem::StageEvaluation{{result.output}, std::move(stats)};
    };
    return {std::move(descriptor), std::move(functions)};
}

// Native common-radius AM/AM and AM/PM; all callbacks own the supplied table.
// The RF small-signal S matrix is the zero-drive derivative, not a large-signal
// gain or a vendor DataPoints interpretation. No intrinsic noise is inferred.
inline ModelAdapter
adapt_tabulated_amplifier(std::string name,
                          TabulatedAmplifier amplifier,
                          double reference_ohms = 50.,
                          AmplifierInputMode mode = AmplifierInputMode::sources_only) {
    if (mode != AmplifierInputMode::sources_only &&
        mode != AmplifierInputMode::propagate_distortion) {
        throw std::invalid_argument("invalid tabulated amplifier input mode");
    }
    ModelDescriptor descriptor{{"rfmodel.tabulated_amplifier", "1", "", ""},
                               std::move(name),
                               {{0, "in", reference_ohms}, {1, "out", reference_ohms}}};
    auto owned = std::make_shared<const TabulatedAmplifier>(std::move(amplifier));
    ModelFunctions functions;
    functions.scattering = [owned](double f) {
        if (f <= 0.) {
            throw std::invalid_argument(
                "tabulated amplifier small-signal S requires positive RF frequency");
        }
        return SMatrix{2, {0., 0., owned->response(0.).gain, 0.}};
    };
    functions.spectrum = [owned](const PowerWaveSpectrum &input) {
        double radius = 0.;
        for (const auto &entry : input.amplitudes) {
            if (entry.first == 0 && entry.second != Complex{}) {
                throw std::invalid_argument("tabulated amplifier cannot transmit nonzero DC");
            }
            radius = std::hypot(radius, std::hypot(entry.second.real(), entry.second.imag()));
        }
        // Validate the common radius even for an empty/zero-only spectrum.
        owned->response(radius);
        auto output = input;
        for (auto &entry : output.amplitudes) {
            entry.second = owned->transmit(entry.second, radius);
        }
        return output;
    };
    functions.operating_point = [owned, reference_ohms](const ModelOperatingPoint &point) {
        return linearize_tabulated_amplifier(
            point.spacing_hz, point.channels, point.incident, *owned, 0, 1, reference_ohms);
    };
    functions.coherent = [owned, reference_ohms, mode](auto &pipeline, const auto &inputs) {
        const auto &snapshot = pipeline.inspect(inputs.at(0));
        if (mode == AmplifierInputMode::sources_only) {
            for (const auto &record : snapshot.records) {
                if (record.component.kind != SpectrumKind::source) {
                    throw std::invalid_argument("tabulated source-only adapter rejects distortion");
                }
            }
        }
        const double power = snapshot.reduction.total_power_w;
        const auto gain = owned->response(std::sqrt(power)).gain;
        // Apply the accepted common gain through the existing linear operation:
        // it scales each history contribution, including cancelled paths whose
        // individual amplitudes can exceed the physical common drive radius.
        auto output =
            pipeline.linear_network({{inputs.at(0), 0}}, {0, 1}, 1, [gain, reference_ohms](double) {
                LinearNetwork network(reference_ohms);
                network.add_power_waves({2, {0., 0., gain, 0.}}, {reference_ohms, reference_ohms});
                return network;
            });
        CoherentSystem::StageStatistics statistics;
        statistics.input_power_w = power;
        statistics.common_amplitude_gain = std::abs(gain);
        statistics.common_complex_gain = gain;
        return CoherentSystem::StageEvaluation{{output}, std::move(statistics)};
    };
    return {std::move(descriptor), std::move(functions)};
}
} // namespace rfmodel
