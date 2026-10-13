// Standalone C++17 SDK consumer. Link only to RFModel::rfmodel.
// No private headers, test helpers, C ABI, JSON, or Python are required.
#include <rfmodel/conversion/conversion_network.hpp>
#include <rfmodel/conversion/conversion_power_wave.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// A second translation unit uses the same installed inline API to check ODR safety.
bool conversion_api_odr_probe();

namespace {
namespace rf = rfmodel;
using rf::Complex;
using Vector = std::vector<Complex>;

constexpr double spacing_hz = 1e8, z0 = 50., resistance = 25.;
constexpr double temperature_k = 290., boltzmann = 1.380649e-23;
constexpr double source_variance = 1e-20, pump_phase = .4;
constexpr std::size_t count = 7;
const Complex carrier{.01, .005};
const std::array<std::size_t, 2> outputs{4, 6};

std::size_t checks = 0;

void require(bool condition, const std::string &what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(what);
    }
}

void close(Complex actual, Complex expected, const std::string &what, bool noise = false) {
    const double error = std::abs(actual - expected);
    const double tolerance = (noise ? 2e-31 : 2e-13) + 2e-10 * std::abs(expected);
    ++checks;
    if (!std::isfinite(error) || error > tolerance) {
        std::ostringstream message;
        message << std::setprecision(17) << what << ": actual=" << actual
                << ", expected=" << expected << ", absolute error=" << error
                << ", tolerance=" << tolerance;
        throw std::runtime_error(message.str());
    }
}

struct Physical {
    Vector voltage = Vector(count), current = Vector(count);
};

// Independent circuit solution, without an RFModel solver or reference transform.
// The ideal mixer supplies two 50 ohm Thevenin sources. A positive series
// voltage eta adds to each source; its Johnson PSD is 4*k*T*R V^2/Hz.
Physical circuit(Complex source, Complex lower_eta = {}, Complex upper_eta = {}) {
    const Complex pump = std::polar(1., pump_phase);
    const std::array<Complex, 2> generated{pump * std::conj(source), pump * source};
    const std::array<Complex, 2> eta{lower_eta, upper_eta};
    Physical result;
    result.voltage[0] = std::sqrt(z0) * source;
    result.current[0] = source / std::sqrt(z0);
    for (std::size_t k = 0; k < 2; ++k) {
        const auto mixer = 1 + k, input = 3 + 2 * k, output = input + 1;
        const auto source_voltage = 2. * std::sqrt(z0) * generated[k];
        const auto current = (source_voltage + eta[k]) / (2. * z0 + resistance);
        result.voltage[mixer] = result.voltage[input] = source_voltage - z0 * current;
        result.voltage[output] = z0 * current;
        result.current[input] = current;
        result.current[mixer] = result.current[output] = -current;
    }
    return result;
}

struct Waves {
    Vector incident = Vector(count), outgoing = Vector(count);
};

// Kurokawa definitions applied directly to independently solved physical V/I.
Waves waves(const Physical &physical, const Vector &references) {
    Waves result;
    for (std::size_t i = 0; i < count; ++i) {
        const auto z = references[i];
        const double divisor = 2. * std::sqrt(z.real());
        result.incident[i] = (physical.voltage[i] + z * physical.current[i]) / divisor;
        result.outgoing[i] = (physical.voltage[i] - std::conj(z) * physical.current[i]) / divisor;
    }
    return result;
}

struct Experiment {
    rf::FrequencyConversionNetwork network{spacing_hz, 75.};
    Vector references, affine_offset;
};

Experiment make_experiment(double reference_scale) {
    // Channel order: mixer RF@8, IF@2, IF@18; resistor ports 0/1@2, 0/1@18.
    const auto mixer =
        rf::ideal_mixer_conversion(spacing_hz, {{0, 8}, {1, 2}, {1, 18}}, 10, 0., pump_phase);
    Vector mixer_refs{{31., 12.}, {73., -21.}, {19., 8.}};
    Vector resistor_refs{{110., 31.}, {43., -12.}, {22., 7.}, {91., -18.}};
    for (auto *refs : {&mixer_refs, &resistor_refs}) {
        for (auto &reference : *refs) {
            reference *= reference_scale;
        }
    }

    auto source_noise = mixer.zero_noise();
    source_noise.covariance(0, 0) = source_variance;
    const auto mixer_change = rf::renormalize_power_waves(mixer, mixer_refs);
    // Only the RF input is external. Leaving IF channels unselected preserves
    // zero boundary placeholders for physical wires, rather than adding loads.
    const auto mixer_boundary = rf::renormalize_conversion_boundary(
        mixer, mixer_refs, {0}, {carrier, {}, {}}, Vector(3), source_noise);

    const double r = resistance / (2. * z0 + resistance), t = 1. - r;
    const rf::SMatrix resistor{2, {r, t, t, r}};
    const auto thermal = rf::passive_thermal_noise(resistor, temperature_k);
    const auto linear =
        rf::lift_linear_conversion(spacing_hz, {2, 18}, {resistor, resistor}, {thermal, thermal});
    const auto resistor_change = rf::renormalize_power_waves(linear.model, resistor_refs);
    // These physical 50 ohm output loads acquire nonzero local reflections.
    const auto resistor_boundary = rf::renormalize_conversion_boundary(
        linear.model, resistor_refs, {1, 3}, linear.source, linear.reflection, linear.source_noise);

    Experiment experiment;
    const auto mixer_id = experiment.network.add_power_waves({mixer_change.model,
                                                              mixer_boundary.source,
                                                              mixer_boundary.reflection,
                                                              mixer_boundary.source_noise,
                                                              mixer.zero_noise()});
    const auto resistor_id = experiment.network.add_power_waves(
        {resistor_change.model,
         resistor_boundary.source,
         resistor_boundary.reflection,
         resistor_boundary.source_noise,
         rf::renormalize_conversion_noise(resistor_change, linear.intrinsic_noise)});
    experiment.network.connect(mixer_id, 1, resistor_id, 0);
    require(experiment.network.channel_offset(mixer_id) == 0 &&
                experiment.network.channel_offset(resistor_id) == 3 &&
                experiment.network.channel_count() == count,
            "network channel layout");
    experiment.references = mixer_refs;
    experiment.references.insert(
        experiment.references.end(), resistor_refs.begin(), resistor_refs.end());
    require(experiment.network.reference_impedances_ohms() == experiment.references,
            "network must retain every local reference, not its 75 ohm default");

    // A separate affine probe supplies known series voltages at both bands.
    const Complex lower_eta{.003, -.001}, upper_eta{-.002, .004};
    const double coefficient = std::sqrt(z0) / (2. * z0 + resistance);
    const auto offset = rf::renormalize_conversion_offset(resistor_change,
                                                          {-coefficient * lower_eta,
                                                           coefficient * lower_eta,
                                                           -coefficient * upper_eta,
                                                           coefficient * upper_eta});
    experiment.affine_offset = Vector(3);
    experiment.affine_offset.insert(experiment.affine_offset.end(), offset.begin(), offset.end());
    return experiment;
}

void check_signal(const rf::ConversionResult &result,
                  const Vector &references,
                  const Physical &expected) {
    require(result.incident.size() == count && result.outgoing.size() == count,
            "signal dimensions");
    require(std::isfinite(result.relative_residual) && result.relative_residual >= 0. &&
                result.relative_residual < 1e-12,
            "conversion solve residual");
    const auto expected_waves = waves(expected, references);
    for (std::size_t i = 0; i < count; ++i) {
        const auto label = "channel " + std::to_string(i);
        close(result.incident[i], expected_waves.incident[i], label + " incident");
        close(result.outgoing[i], expected_waves.outgoing[i], label + " outgoing");
        const auto z = references[i];
        const auto voltage =
            (std::conj(z) * result.incident[i] + z * result.outgoing[i]) / std::sqrt(z.real());
        const auto current = (result.incident[i] - result.outgoing[i]) / std::sqrt(z.real());
        close(voltage, expected.voltage[i], label + " physical voltage");
        close(current, expected.current[i], label + " physical current");
        close(std::norm(result.incident[i]) - std::norm(result.outgoing[i]),
              (expected.voltage[i] * std::conj(expected.current[i])).real(),
              label + " net signal power");
    }
}

void check_noise(const rf::ConversionResult &result, const Vector &references) {
    // Six independent real quadratures: proper RF source noise, and the
    // resistor's independent Johnson voltage noise at 200 MHz and 1.8 GHz.
    // This oracle never calls an RFModel noise/renormalization helper.
    const Complex j{0., 1.};
    const std::array<Waves, 6> basis{waves(circuit(1.), references),
                                     waves(circuit(j), references),
                                     waves(circuit({}, 1.), references),
                                     waves(circuit({}, j), references),
                                     waves(circuit({}, {}, 1.), references),
                                     waves(circuit({}, {}, j), references)};
    const double johnson_variance = 4. * boltzmann * temperature_k * resistance;
    const std::array<double, 6> weights{source_variance / 2.,
                                        source_variance / 2.,
                                        johnson_variance / 2.,
                                        johnson_variance / 2.,
                                        johnson_variance / 2.,
                                        johnson_variance / 2.};
    for (const auto *noise :
         {&result.outgoing_noise, &result.incident_noise, &result.incident_outgoing_noise}) {
        require(noise->covariance.ports == count && noise->complementary.ports == count,
                "loaded noise dimensions");
    }
    require(result.net_noise_into_device_w_per_hz.size() == count, "net noise dimensions");
    for (std::size_t row = 0; row < count; ++row) {
        for (std::size_t column = 0; column < count; ++column) {
            Complex caa{}, paa{}, cbb{}, pbb{}, cab{}, pab{};
            for (std::size_t k = 0; k < basis.size(); ++k) {
                const auto a = basis[k].incident[row], b = basis[k].outgoing[row];
                const auto other_a = basis[k].incident[column];
                const auto other_b = basis[k].outgoing[column];
                caa += weights[k] * a * std::conj(other_a);
                paa += weights[k] * a * other_a;
                cbb += weights[k] * b * std::conj(other_b);
                pbb += weights[k] * b * other_b;
                cab += weights[k] * a * std::conj(other_b);
                pab += weights[k] * a * other_b;
            }
            const auto label = "[" + std::to_string(row) + "," + std::to_string(column) + "]";
            close(result.incident_noise.covariance(row, column), caa, "Caa" + label, true);
            close(result.incident_noise.complementary(row, column), paa, "Paa" + label, true);
            close(result.outgoing_noise.covariance(row, column), cbb, "Cbb" + label, true);
            close(result.outgoing_noise.complementary(row, column), pbb, "Pbb" + label, true);
            close(result.incident_outgoing_noise.covariance(row, column), cab, "Cab" + label, true);
            close(result.incident_outgoing_noise.complementary(row, column),
                  pab,
                  "Pab" + label,
                  true);
            if (row == column) {
                close(result.net_noise_into_device_w_per_hz[row],
                      caa - cbb,
                      "net noise " + label,
                      true);
            }
        }
    }
    for (const auto &connection : {std::pair<std::size_t, std::size_t>{1, 3}, {2, 5}}) {
        close(result.net_noise_into_device_w_per_hz[connection.first] +
                  result.net_noise_into_device_w_per_hz[connection.second],
              0.,
              "wire net noise conservation",
              true);
    }
}

void run_example(double reference_scale) {
    const auto experiment = make_experiment(reference_scale);
    const auto result = experiment.network.analyze(true);
    check_signal(result, experiment.references, circuit(carrier));
    check_noise(result, experiment.references);
    check_signal(experiment.network.analyze(true, nullptr, experiment.affine_offset),
                 experiment.references,
                 circuit(carrier, {.003, -.001}, {-.002, .004}));

    // Explicit NF experiment: RF channel 0 is both reference and thermal band.
    // Output loads remain cold. NF replaces the arbitrary operating source PSD.
    const double kt = boltzmann * temperature_k;
    const double t = 2. * z0 / (2. * z0 + resistance);
    const double thermal =
        4. * kt * resistance * z0 / ((2. * z0 + resistance) * (2. * z0 + resistance));
    const double expected_factor = 1. + thermal / (kt * t * t);
    std::array<Complex, 2> common_output{}, output_transform{};
    for (std::size_t k = 0; k < outputs.size(); ++k) {
        const auto output = outputs[k];
        const auto z = experiment.references[output];
        // Each output has a physical cold 50 ohm load, so b50=V/sqrt(50).
        output_transform[k] = 2. * std::sqrt(z0 * z.real()) / (z0 + std::conj(z));
        common_output[k] = output_transform[k] * result.outgoing[output];
        const auto nf =
            experiment.network.reference_noise_analysis({0}, {0}, output, temperature_k);
        close(nf.reference_gain, t * t, "NF reference gain");
        close(nf.reference_output_noise_w_per_hz, kt * t * t, "NF reference PSD", true);
        close(nf.output_noise_w_per_hz, kt * t * t + thermal, "NF total PSD", true);
        close(nf.noise_factor, expected_factor, "explicit noise factor");
        close(nf.noise_figure_db, 10. * std::log10(expected_factor), "explicit NF dB");
        close(nf.equivalent_input_temperature_k,
              temperature_k * (expected_factor - 1.),
              "equivalent input noise temperature");
        close(-result.net_noise_into_device_w_per_hz[output],
              t * t * source_variance + thermal,
              "physical load noise PSD",
              true);
    }
    const Complex pump = std::polar(1., pump_phase);
    close(common_output[0], t * pump * std::conj(carrier), "folded lower sideband");
    close(common_output[1], t * pump * carrier, "upper sideband");
    close(output_transform[0] * result.outgoing_noise.complementary(outputs[0], outputs[1]) *
              output_transform[1],
          t * t * pump * pump * source_variance,
          "sideband complementary correlation",
          true);
    std::cout << "reference scale " << reference_scale << ": b50(lower)=" << common_output[0]
              << ", b50(upper)=" << common_output[1] << ", gain=" << t * t
              << ", NF=" << 10. * std::log10(expected_factor) << " dB\n";
}
} // namespace

int main() {
    try {
        require(conversion_api_odr_probe(), "conversion API multi-translation-unit probe");
        std::cout << std::setprecision(12);
        for (const double scale : {.5, 1., 2.}) {
            run_example(scale);
        }
        std::cout << "PASS: " << checks << " native C++ conversion API checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "conversion API consumer: " << error.what() << '\n';
        return 1;
    }
}
