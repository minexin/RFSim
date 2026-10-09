#include "rfmodel/frequency_conversion.hpp"
#include "rfmodel/chebyshev_filter.hpp"
#include "rfmodel/butterworth_filter.hpp"
#include "rfmodel/power_wave_noise.hpp"
#include <rfmodel/power_wave_reference.hpp>
#include <rfmodel/intermod_levels.hpp>
#include <rfmodel/coherent_highorder_amplifier.hpp>
#include <rfmodel/polynomial_intercepts.hpp>
#include <rfmodel/origin_expression.hpp>
#include <rfmodel/mixing_origin.hpp>
#include <rfmodel/coherent_polynomial.hpp>
#include <rfmodel/coherent_amplifier.hpp>
#include <rfmodel/coherent_mixer.hpp>
#include <rfmodel/coherent_compression.hpp>
#include <rfmodel/source_coherence.hpp>
#include <rfmodel/coherent_network.hpp>
#include <rfmodel/coherence.hpp>
#include <rfmodel/linear_analysis.hpp>
#include <rfmodel/interpolation.hpp>
#include <rfmodel/measurements.hpp>
#include <rfmodel/noise.hpp>
#include <rfmodel/noise_figure.hpp>
#include <rfmodel/linear_solver.hpp>
#include <rfmodel/power_gain.hpp>
#include <rfmodel/linear_path_noise.hpp>
#include <rfmodel/loaded_noise.hpp>
#include <rfmodel/transmission_line.hpp>
#include <rfmodel/rlgc_transmission_line.hpp>
#include <rfmodel/multiport_devices.hpp>
#include <cmath>
#include <rfmodel/fundamental_compression.hpp>
#include <rfmodel/saturating_fundamental.hpp>
#include <rfmodel/single_tone_amplifier.hpp>
#include <rfmodel/multitone_amplifier.hpp>
#include <rfmodel/term_propagation.hpp>

int main() {
    {
        rfmodel::FrequencyConversionModel converter(1e9, {{0, 1}}, {1, {.5}}, {1, {0.}});
        const auto zero = converter.zero_noise();
        if (std::abs(converter.analyze({2.}, {0.}, zero, zero).outgoing[0] - 1.) > 1e-12) {
            return 90;
        }
    }

    {
        rfmodel::ChebyshevFilterModel filter("filter", {});
        if (std::abs(filter.s_parameters(0.)(1, 0) - 1.) > 1e-12) {
            return 89;
        }
    }

    {
        rfmodel::ButterworthFilterModel filter("filter", {});
        if (std::abs(filter.s_parameters(0.)(1, 0) - 1.) > 1e-12) {
            return 89;
        }
    }

    {
        const rfmodel::SMatrix pad{2, {0., .5, .5, 0.}};
        const auto noise = rfmodel::passive_thermal_noise(pad, 290.);
        const auto parameters =
            rfmodel::extract_power_wave_noise_parameters(pad, noise, {50., 50.});
        if (std::abs(parameters.noise_resistance_ohms - 46.875) > 1e-10) {
            return 87;
        }
    }

    const auto references = rfmodel::renormalize_power_waves(
        rfmodel::SMatrix{1, {0.}}, {50.}, {rfmodel::Complex{75., 20.}});
    const auto impedance =
        rfmodel::s_to_z(references.scattering, std::vector<rfmodel::Complex>{{75., 20.}});
    if (std::abs(impedance(0, 0) - 50.) > 1e-11) {
        return 86;
    }
    const auto im_coefficients =
        rfmodel::polynomial_coefficients_from_intermod_levels(10., {0., -40., -60.}, {1, -1});
    if (im_coefficients.size() != 4 || im_coefficients[3] >= 0.) {
        return 85;
    }
    const auto high_order = rfmodel::CoherentHighOrderAmplifier(10., 20., 23., {0., 0., .01})
                                .evaluate(1e8, {{10, rfmodel::SpectrumKind::source, 1., 7, .01}});
    if (high_order.terms.size() != 3 || high_order.terms.back().order != 4) {
        return 84;
    }
    const auto intercept_coefficients = rfmodel::polynomial_coefficients_from_intercepts(
        10., {{3, -1, 33., 1, rfmodel::InterceptReference::output}});
    if (intercept_coefficients.size() != 5 ||
        std::abs(intercept_coefficients[4] - .07096267784671509) > 1e-14) {
        return 82;
    }

    const auto point =
        rfmodel::CoherentLimitedAmplifier(20., 20., 23., 20., 10.).operating_point(0.);
    if (point.fundamental_amplitude_gain != 10. || point.nonlinear_input_scale != 1.) {
        return 79;
    }
    const rfmodel::OriginExpression expressions{{{{7, 1}}, 1.}, {{{9, 1}}, -1.}};
    const auto expression = rfmodel::product_origin_expressions({expressions}, {1, 1});
    if (expression.size() != 3 ||
        rfmodel::origin_expression_amplitude(expression) != rfmodel::Complex{}) {
        return 75;
    }

    const auto origin = rfmodel::expand_mixing_origin({{{7, 1}, {9, -1}}}, {-1});
    if (origin != rfmodel::MixingOrigin{{7, -1}, {9, 1}}) {
        return 73;
    }

    static_assert(rfmodel::maximum_polynomial_order == 11, "installed order limit");
    std::vector<double> eleven_coefficients(12, 0.);
    eleven_coefficients.back() = 1.;
    const auto eleventh = rfmodel::CoherentPolynomial(eleven_coefficients)
                              .evaluate(1e8, {{10, rfmodel::SpectrumKind::source, 1., 7, .01}});
    if (eleventh.terms.size() != 6 || eleventh.terms.back().input_indices[10] != 1 ||
        eleventh.terms.back().component.bin != 110) {
        return 83;
    }
    const auto ninth = rfmodel::CoherentPolynomial({0., 0., 0., 0., 0., 0., 0., 0., 0., 1.})
                           .evaluate(1e8, {{10, rfmodel::SpectrumKind::harmonic, 1., 7, .01}});
    if (ninth.terms.size() != 5 || ninth.terms.back().input_indices[8] != 1) {
        return 71;
    }

    const auto coherent_products = rfmodel::CoherentLimitedAmplifier(20., 20., 23., 20., 10.)
                                       .evaluate(1e8,
                                                 {{10, rfmodel::SpectrumKind::source, 1., 7, .01},
                                                  {10, rfmodel::SpectrumKind::source, 1., 9, .01}});
    if (coherent_products.terms.size() != 15 || coherent_products.inputs.size() != 2) {
        return 30;
    }

    const auto propagated_products =
        rfmodel::CoherentLimitedAmplifier(20., 20., 23., 20., 10.)
            .evaluate_cascade(1e8, {{20, rfmodel::SpectrumKind::harmonic, 2., 7, .01}});
    if (propagated_products.terms.size() != 1 ||
        propagated_products.terms[0].component.kind != rfmodel::SpectrumKind::harmonic) {
        return 31;
    }
    const auto coherent_compressed = rfmodel::compress_coherent_fundamentals(
        1e8,
        {{10, rfmodel::SpectrumKind::source, 1., 7, .001},
         {10, rfmodel::SpectrumKind::source, 1., 7, -.001}},
        rfmodel::SaturatingFundamentalCompression(20., 20., 23.));
    if (coherent_compressed.input_power_w != 0. || coherent_compressed.output.total_power_w != 0. ||
        coherent_compressed.output.components.size() != 1) {
        return 29;
    }
    const auto mixed = rfmodel::mix_coherent_components(
        1e8, {{{10, rfmodel::SpectrumKind::source, 1., 7, 1.}, 8, 0., 0., 9}});
    if (mixed.size() != 2 || mixed[0].bin != 2 || mixed[1].bin != 18 ||
        std::abs(mixed[0].amplitude - 1.) > 1e-12 || mixed[0].coherence_group <= 9) {
        return 28;
    }
    const auto source_groups = rfmodel::assign_source_coherence({{"a", "clock"}, {"b", "clock"}});
    if (source_groups.size() != 2 || source_groups[0] != source_groups[1] ||
        source_groups[0] == 0) {
        return 27;
    }
    const auto combined = rfmodel::transmit_coherent_network(
        1e8,
        {{0, {10, rfmodel::SpectrumKind::source, 1., 7, 1.}},
         {1, {10, rfmodel::SpectrumKind::source, 1., 7, -1.}}},
        {2, 1, 0},
        2,
        50.,
        [](double) {
            rfmodel::LinearNetwork network;
            network.add({3, {0., 0., .5, 0., 0., .5, .5, .5, 0.}});
            return network;
        });
    if (combined.components.size() != 1 || combined.total_power_w != 0.) {
        return 26;
    }
    const auto coherence =
        rfmodel::reduce_coherent_components(1e8,
                                            {{10, rfmodel::SpectrumKind::source, 1., 1, 1.},
                                             {10, rfmodel::SpectrumKind::source, 1., 1, -1.}});
    if (coherence.components.size() != 1 || coherence.total_power_w != 0.) {
        return 25;
    }

    const rfmodel::MultiToneLimitedAmplifier multitone(20., 20., 23., 20., 10.);
    const auto traced = multitone.evaluate_terms({1e8, {{10, .001}, {11, .001}}});
    const auto propagated =
        rfmodel::transmit_linear_terms(1e8, traced.terms, {0, 1}, 50., [](double) {
            rfmodel::LinearNetwork network;
            network.add({2, {0., .5, .5, 0.}});
            return network;
        });
    if (propagated.size() != traced.terms.size() ||
        std::abs(propagated[7].amplitude / traced.terms[7].amplitude - .5) > 1e-12) {
        return 22;
    }
    if (traced.terms.size() != 16 || traced.terms[7].contributors[0] != -11 ||
        std::abs(traced.terms[7].amplitude / -2e-6 - 1.) > 1e-12) {
        return 21;
    }
    const auto families = multitone.evaluate({1e8, {{10, .001}, {11, .001}}});
    if (std::abs(std::norm(families.third_order.amplitudes.at(9)) / 1e-12 - 1.) > 1e-12 ||
        families.direct.amplitudes.size() != 2 || !families.third_order.amplitudes.count(10)) {
        return 20;
    }
    const rfmodel::SingleToneLimitedAmplifier single_tone(20., 20., 23., 20., 10.);
    const auto single_tone_output = single_tone.transmit({1e6, {{10, .001}}});
    if (single_tone_output.amplitudes.size() != 3 ||
        std::abs(std::norm(single_tone_output.amplitudes.at(20)) / 2.5e-10 - 1.) > 1e-12) {
        return 19;
    }
    const rfmodel::SaturatingFundamentalCompression saturation(20., 20., 23.);
    if (std::abs(std::norm(saturation.transmit_fundamental(.1)) / .19922937036162172 - 1.) >
        1e-12) {
        return 18;
    }
    const rfmodel::P1dBFundamentalCompression compressed(20., 10.);
    const std::vector<rfmodel::PowerWaveSpectrum> rf_inputs{{1e6, {{10, .001}, {20, .002}}},
                                                            {1e6, {{10, .003}}}};
    if (std::abs(rfmodel::incident_rf_power_watts(rf_inputs) / 14e-6 - 1.) > 1e-12 ||
        std::abs(compressed.transmit_fundamental_from_spectra(rf_inputs, 0, 10) -
                 compressed.transmit_fundamental(.001, 14e-6)) > 1e-12) {
        return 14;
    }
    const auto driven = compressed.transmit_fundamental(
        std::sqrt(compressed.input_p1db_watts() / 4.), compressed.input_p1db_watts());
    if (std::abs(std::norm(driven) / .0025 - 1.) > 1e-12) {
        return 12;
    }
    if (std::abs(
            std::norm(compressed.transmit_fundamental(std::sqrt(compressed.input_p1db_watts()))) /
                .01 -
            1.) > 1e-12) {
        return 11;
    }
    const auto loaded = rfmodel::loaded_noise({1, {0.}}, {{1, {1.}}}, {0.5}, {{1, {0.}}});
    if (std::abs(loaded.net_into_device_w_per_hz[0] + 0.75) > 1e-12) {
        return 10;
    }
    const rfmodel::RlgcTransmissionLineModel distributed("distributed", {25., 0., 0., 0.}, 2.);
    if (std::abs(distributed.s_parameters(0.)(1, 0) - rfmodel::Complex{2. / 3., 0.}) > 1e-12) {
        return 9;
    }
    const rfmodel::IsolatedPowerDividerModel phased("phased", {0.5, rfmodel::Complex{0., -0.5}});
    if (std::abs(phased.s_parameters(1e9)(2, 0) - rfmodel::Complex{0., -0.5}) > 1e-12) {
        return 8;
    }
    const rfmodel::EqualPowerDividerModel divider("divider", 4);
    const rfmodel::QuadratureCouplerModel coupler("coupler", 0.25);
    if (std::abs(divider.s_parameters(1e9)(1, 0) - rfmodel::Complex{0.5, 0.}) > 1e-12 ||
        std::abs(coupler.s_parameters(1e9)(2, 0) - rfmodel::Complex{0., 0.5}) > 1e-12) {
        return 7;
    }
    const rfmodel::TransmissionLineModel line("quarter wave", 100., 0.25e-9);
    if (std::abs(line.s_parameters(1e9)(0, 0) - rfmodel::Complex{0.6, 0.}) > 1e-12) {
        return 6;
    }
    auto result = rfmodel::analyze_linear(
        rfmodel::FrequencyGrid{{1e9}},
        {0, 1},
        [](double) {
            rfmodel::LinearNetwork network;
            network.add(rfmodel::SMatrix{2, {0., 0.5, 0.5, 0.}});
            return network;
        },
        [](double, const rfmodel::LinearNetwork &) {
            return rfmodel::passive_thermal_noise(rfmodel::SMatrix{2, {0., 0.5, 0.5, 0.}}, 290.);
        });
    if (std::abs(result.scattering[0](1, 0) - rfmodel::Complex{0.5, 0}) >= 1e-12) {
        return 1;
    }
    const auto parameters =
        rfmodel::extract_noise_parameters(result.scattering[0], result.noise_correlation[0]);
    const auto imported = rfmodel::noise_from_parameters(result.scattering[0], parameters);
    const auto nf = rfmodel::two_port_noise_figure_db(result.scattering[0], imported);
    const auto chain = rfmodel::analyze_linear_path_noise(
        {{"first", result.scattering[0]}, {"second", result.scattering[0]}}, {imported, imported});
    if (chain.contributions.size() != 2 || chain.contributions[0].name != "first" ||
        chain.contributions[1].name != "second") {
        return 5;
    }
    if (!chain.noise_factor || std::abs(*chain.noise_factor - 16.) > 1e-10 ||
        std::abs(chain.total_output_w_per_hz / (1.380649e-23 * 290.) - 1.) > 1e-10) {
        return 4;
    }
    if (std::abs(rfmodel::operating_power_gain(result.scattering[0]) - 0.25) >= 1e-12) {
        return 3;
    }
    return std::abs(nf - 10 * std::log10(4.)) < 1e-12 ? 0 : 2;
}
