#pragma once
#include "rfmodel/adapters/model_adapter.hpp"
#include "rfmodel/conversion/amplifier_linearization.hpp"
#include "rfmodel/conversion/highorder_linearization.hpp"
#include "rfmodel/conversion/polynomial_linearization.hpp"
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
} // namespace rfmodel
