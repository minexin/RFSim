#include "rfmodel/io/tabulated_noise_model.hpp"
#include "rfmodel/conversion/conversion_power_wave.hpp"
#include "rfmodel/conversion/conversion_network.hpp"
#include "../support/test_support.hpp"
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>

static std::size_t conversion_chain_checks = 0;
static double maximum_conversion_chain_scaled_error = 0.;

static void test_complex_reference_conversion_chain(const char *path) {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290.;
    const auto value = [](Complex actual, Complex expected, double scale, const char *message) {
        require(std::isfinite(scale) && scale > 0., "invalid Touchstone fixture scale");
        const double error = std::abs(actual - expected) / scale;
        require(std::isfinite(error) && error < 3e-12, message);
        maximum_conversion_chain_scaled_error =
            std::max(maximum_conversion_chain_scaled_error, error);
        ++conversion_chain_checks;
    };
    const auto model = TabulatedNoiseModel::from_touchstone("75 ohm noisy amplifier", path);
    // Interpolate both tables between their 1 and 3 GHz endpoints.
    const auto s75 = model.s_parameters(2e9);
    const auto c75 = model.noise_correlation(2e9);
    const std::vector<Complex> old_refs{model.port(0).reference_impedance,
                                        model.port(1).reference_impedance};
    require(old_refs == std::vector<Complex>{75., 75.}, "embedded Touchstone reference lost");
    const SMatrix expected_s75{2, {0., 0., 2., 0.}};
    // For S21=2, GammaOpt=0, Fmin=2, Rn/Z0=1/2, matching coefficients
    // of F(Gamma)=2+2*|Gamma|^2/(1-|Gamma|^2) gives C75/kT=diag(1,4).
    const SMatrix expected_c75{2, {kt, 0., 0., 4. * kt}};
    for (std::size_t i = 0; i < 4; ++i) {
        value(s75.values[i], expected_s75.values[i], 1., "Touchstone amplifier scattering");
        value(c75.watts_per_hz.values[i],
              expected_c75.values[i],
              kt,
              "Touchstone intrinsic noise from independent noise-factor polynomial");
    }
    const std::vector<Complex> refs{{31., 12.}, {73., -21.}};
    const auto local_s = renormalize_power_waves(s75, old_refs, refs).scattering;
    const auto local_c = renormalize_noise(s75, c75, old_refs, refs);
    auto device = lift_linear_conversion(
        2e9, {1}, {local_s}, {local_c}, std::vector<std::vector<Complex>>{refs});
    // Cold physical 50-ohm terminations, expressed independently in local waves.
    // a=Gamma*b+g*q, where q is the incoming wave at a physical 50-ohm reference.
    const std::vector<Complex> q{{.02, -.01}, {-.03, .015}};
    std::vector<Complex> gain(2), gamma(2);
    for (std::size_t i = 0; i < 2; ++i) {
        gain[i] = 2. * std::sqrt(50. * refs[i].real()) / (50. + std::conj(refs[i]));
        gamma[i] = (50. - refs[i]) / (50. + std::conj(refs[i]));
        device.source[i] = gain[i] * q[i];
        device.reflection[i] = gamma[i];
    }
    FrequencyConversionNetwork network(2e9);
    network.add_power_waves(device);
    const auto result = network.analyze(true);
    require(network.reference_impedances_ohms() == refs, "Touchstone lift reference order");
    require(device.model.channels()[0].port == 0 && device.model.channels()[0].bin == 1 &&
                device.model.channels()[1].port == 1 && device.model.channels()[1].bin == 1,
            "Touchstone lift channel identity");
    // At 75 ohms Gamma_load=-1/5. Hence b75=(c1, c2-2*c1/5).
    // In physical 50-ohm outgoing coordinates, b50=sqrt(24/25)*b75.
    // These constants follow from that two-equation physical feedback solve,
    // independent of the renormalization and loaded-noise implementation.
    const SMatrix c50{2, {.96 * kt, -.384 * kt, -.384 * kt, 3.9936 * kt}};
    const std::vector<Complex> b50{.2 * q[0], 1.92 * q[0] + .2 * q[1]};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto voltage = std::sqrt(50.) * (q[i] + b50[i]);
        const auto current = (q[i] - b50[i]) / std::sqrt(50.);
        const auto expected_a = (voltage + refs[i] * current) / (2. * std::sqrt(refs[i].real()));
        const auto expected_b =
            (voltage - std::conj(refs[i]) * current) / (2. * std::sqrt(refs[i].real()));
        value(result.incident[i], expected_a, .1, "Touchstone loaded physical incident wave");
        value(result.outgoing[i], expected_b, .1, "Touchstone loaded physical outgoing wave");
        value(result.net_noise_into_device_w_per_hz[i],
              -c50(i, i),
              kt,
              "Touchstone physical 50-ohm net noise");
        for (std::size_t j = 0; j < 2; ++j) {
            const auto expected_b_noise = c50(i, j) / (gain[i] * std::conj(gain[j]));
            value(result.outgoing_noise.covariance(i, j),
                  expected_b_noise,
                  kt,
                  "Touchstone full loaded outgoing covariance");
            value(result.incident_noise.covariance(i, j),
                  gamma[i] * expected_b_noise * std::conj(gamma[j]),
                  kt,
                  "Touchstone full loaded incident covariance");
            value(result.incident_outgoing_noise.covariance(i, j),
                  gamma[i] * expected_b_noise,
                  kt,
                  "Touchstone full loaded incident-outgoing covariance");
            // The loaded solver forms complex moments from real quadratures;
            // cancellation roundoff is bounded in kT units, without a unit floor.
            value(result.outgoing_noise.complementary(i, j),
                  0.,
                  kt,
                  "proper Touchstone outgoing noise acquired complementary correlations");
            value(result.incident_noise.complementary(i, j),
                  0.,
                  kt,
                  "proper Touchstone incident noise acquired complementary correlations");
            value(result.incident_outgoing_noise.complementary(i, j),
                  0.,
                  kt,
                  "proper Touchstone cross noise acquired complementary correlations");
            value(gain[i] * result.outgoing_noise.covariance(i, j) * std::conj(gain[j]),
                  c50(i, j),
                  kt,
                  "Touchstone restored 50-ohm covariance");
        }
    }
    require(model.s_parameters(2e9).values == s75.values &&
                model.noise_correlation(2e9).watts_per_hz.values == c75.watts_per_hz.values,
            "conversion chain mutated owned Touchstone samples");
}

int main() {
    using namespace rfmodel;
    const char *path = "touchstone-noise.s2p";
    const std::string network = "# MHz S RI R 75\n1000 0 0 2 0 0 0 0 0\n3000 0 0 2 0 0 0 0 0\n";
    auto write = [&](const std::string &content) {
        std::ofstream output(path);
        output << content;
    };
    write(network +
          "! Noise always uses magnitude/degrees\n1000 3 .5 90 2\n3000 3 .5 -90 2 ! tail\n");
    const auto data = read_touchstone(path);
    require(data.matrices.size() == 2 && data.noise_samples.size() == 2, "separate table sizes");
    near(data.noise_samples[0].frequency_hz, 1e9);
    near(data.noise_samples[0].optimum_source_reflection, Complex{0., .5});
    near(data.noise_samples[0].noise_resistance_ohms, 150.);
    const auto model = TabulatedNoiseModel::from_touchstone("amplifier", path);
    for (double f : {1e9, 3e9}) {
        const auto parameters =
            extract_noise_parameters(model.s_parameters(f), model.noise_correlation(f), 75.);
        near(parameters.minimum_noise_figure_db, 3.);
        near(parameters.noise_resistance_ohms, 150.);
        near(parameters.optimum_source_reflection, Complex{0., f == 1e9 ? .5 : -.5});
    }
    // Same physical fixture as examples/noisy-amplifier.s2p, in MHz units.
    write(network + "1000 3.010299956639812 0 0 0.5\n3000 3.010299956639812 0 0 0.5\n");
    test_complex_reference_conversion_chain(path);
    // Equal first noise/last network frequency is also a legal transition.
    write(network + "3000 3 0 0 2\n");
    require(read_touchstone(path).noise_samples.size() == 1, "equal-frequency transition");
    for (const auto &tail : {"1000 3 .5 90\n",
                             "1000 3 .5 90 2 7\n",
                             "1000 3 .5 90 2\n1000 3 .5 90 2\n",
                             "1000 3 -.5 90 2\n",
                             "1000 3 .5 90 -2\n",
                             "1000 3 .5 90 1e308\n",
                             "1000 3 .5 90 2\n4000 0 0 2 0 0 0 0 0\n"}) {
        write(network + tail);
        rejects<std::runtime_error>([&] {
            read_touchstone(path);
        });
    }
    write(network);
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_touchstone("missing", path);
    });
    // Syntactically valid but physically inconsistent noise parameters.
    write(network + "1000 3 0 0 0\n");
    require(read_touchstone(path).noise_samples.size() == 1, "raw parameters retained");
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_touchstone("invalid", path);
    });
    std::remove(path);
    std::cout << std::setprecision(17)
              << "{\"passed\":true,\"conversion_chain_checks\":" << conversion_chain_checks
              << ",\"maximum_conversion_chain_scaled_error\":"
              << maximum_conversion_chain_scaled_error << "}\n";
}
