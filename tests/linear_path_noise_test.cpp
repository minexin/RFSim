#include "rfmodel/linear_path_noise.hpp"
#include "rfmodel/noise_figure.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290;
    const SMatrix amplifier{2, {0., 0., 10., 0.}};
    const SMatrix attenuator{2, {0., 0.5, 0.5, 0.}};
    const NoiseCorrelation amp_noise{{2, {0., 0., 0., 100. * kt}}};
    const auto pad_noise = passive_thermal_noise(attenuator, 290);
    const std::vector<LinearPathStage> stages{{"amp", amplifier}, {"pad", attenuator}};
    const std::vector<NoiseCorrelation> noises{amp_noise, pad_noise};
    const auto result = analyze_linear_path_noise(stages, noises);
    near(result.transducer_gain, 25.);
    near(result.source_output_w_per_hz / kt, 25.);
    near(result.intrinsic_output_w_per_hz / kt, 25.75);
    near(result.total_output_w_per_hz / kt, 50.75);
    require(result.contributions.size() == 2 && result.contributions[0].name == "amp" &&
                result.contributions[1].name == "pad",
            "contribution stage order");
    near(result.contributions[0].output_w_per_hz / kt, 25.);
    near(result.contributions[1].output_w_per_hz / kt, 0.75);
    // Independent Friis expectation: F1=2, F2=4, G1=100.
    near(*result.noise_factor, 2. + (4. - 1.) / 100.);
    const auto cold = analyze_linear_path_noise(stages, noises, 0.);
    near(cold.source_output_w_per_hz / kt, 0.);
    near(cold.total_output_w_per_hz / kt, 25.75);
    near(*cold.noise_factor, *result.noise_factor);

    // Both passive pads at equilibrium leave a matched thermal spectrum unchanged.
    const auto equilibrium = analyze_linear_path_noise({{"pad1", attenuator}, {"pad2", attenuator}},
                                                       {pad_noise, pad_noise});
    near(equilibrium.total_output_w_per_hz / kt, 1.);
    near(*equilibrium.noise_factor, 16.);

    // Passive-network fluctuation/dissipation identity independently constrains
    // all aggregate covariance entries, including reverse waves and correlations.
    const SMatrix passive{2, {Complex{0.1, 0.1}, 0.4, 0.4, Complex{-0.2, 0.1}}};
    const auto passive_noise = passive_thermal_noise(passive, 290.);
    const auto reflected_chain = analyze_linear_path_noise({{"left", passive}, {"right", passive}},
                                                           {passive_noise, passive_noise});
    const auto equilibrium_covariance = passive_thermal_noise(reflected_chain.scattering, 290.);
    for (std::size_t index = 0; index < 4; ++index) {
        near(reflected_chain.intrinsic.watts_per_hz.values[index] / kt,
             equilibrium_covariance.watts_per_hz.values[index] / kt);
    }

    const SMatrix bilateral{2, {Complex{0.2, 0.1}, 0.1, 2., Complex{-0.1, 0.2}}};
    const NoiseCorrelation correlated{
        {2, {kt, Complex{0., 0.5 * kt}, Complex{0., -0.5 * kt}, 2. * kt}}};
    const Complex source{0.2, -0.1}, load{-0.1, 0.3};
    const auto mismatched =
        analyze_linear_path_noise({{"device", bilateral}}, {correlated}, 290., source, load);
    // Explicit scalar solution: b2 = (S21*Gs*c1 + (1-S11*Gs)*c2)/det.
    const Complex a = bilateral(1, 0) * source;
    const Complex b = 1. - bilateral(0, 0) * source;
    const Complex det =
        b * (1. - bilateral(1, 1) * load) - bilateral(0, 1) * bilateral(1, 0) * source * load;
    const double expected =
        (std::norm(a) + 2. * std::norm(b) + 2. * (a * Complex{0., 0.5} * std::conj(b)).real()) *
        (1. - std::norm(load)) / std::norm(det);
    near(mismatched.intrinsic_output_w_per_hz / kt, expected);
    near(*mismatched.noise_factor,
         std::pow(10., two_port_noise_figure_db(bilateral, correlated, source) / 10.));
    const auto reflecting = analyze_linear_path_noise(stages, noises, 290., {}, 1.);
    require(!reflecting.noise_factor, "reflecting load has no noise factor");
    near(reflecting.total_output_w_per_hz / kt, 0.);
    const auto blocked =
        analyze_linear_path_noise({{"blocked", {2, {0., 0., 0., 0.}}}}, {{{2, {0., 0., 0., kt}}}});
    require(!blocked.noise_factor, "zero transmission has no noise factor");
    near(blocked.total_output_w_per_hz / kt, 1.);
    rejects<std::invalid_argument>([&] {
        analyze_linear_path_noise(stages, {amp_noise});
    });
    rejects<std::invalid_argument>([&] {
        analyze_linear_path_noise(stages, noises, -1.);
    });
    rejects<std::invalid_argument>([&] {
        analyze_linear_path_noise(stages, noises, 290., 1.);
    });
    auto invalid = noises;
    invalid[0].watts_per_hz(0, 0) = -kt;
    rejects<std::invalid_argument>([&] {
        analyze_linear_path_noise(stages, invalid);
    });
}
