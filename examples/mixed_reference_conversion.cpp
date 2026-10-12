#include "rfmodel/conversion_network.hpp"
#include "rfmodel/conversion_power_wave.hpp"
#include <iomanip>
#include <iostream>
#include <string>

using namespace rfmodel;

static void complex_json(Complex value) {
    std::cout << '[' << value.real() << ',' << value.imag() << ']';
}

static void vector_json(const std::vector<Complex> &values) {
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            std::cout << ',';
        }
        complex_json(values[i]);
    }
    std::cout << ']';
}

static void matrix_json(const SMatrix &matrix) {
    std::cout << '[';
    for (std::size_t i = 0; i < matrix.ports; ++i) {
        if (i) {
            std::cout << ',';
        }
        std::vector<Complex> row;
        for (std::size_t j = 0; j < matrix.ports; ++j) {
            row.push_back(matrix(i, j));
        }
        vector_json(row);
    }
    std::cout << ']';
}

int main(int argc, char **argv) {
    try {
        double scale = 1.;
        if (argc > 2) {
            throw std::invalid_argument(
                "usage: mixed_reference_conversion_example [reference_scale]");
        }
        if (argc == 2) {
            std::size_t used = 0;
            const std::string argument(argv[1]);
            scale = std::stod(argument, &used);
            if (used != argument.size() || !std::isfinite(scale) || scale <= 0.) {
                throw std::invalid_argument("reference scale must be finite and positive");
            }
        }
        const double spacing = 1e8, phase = .4, temperature = 290., variance = 1e-20;
        const Complex carrier{.01, .005};
        const auto mixer =
            ideal_mixer_conversion(spacing, {{0, 8}, {1, 2}, {1, 18}}, 10, 0., phase);
        std::vector<Complex> mixer_refs{{31., 12.}, {73., -21.}, {19., 8.}};
        std::vector<Complex> resistor_refs{{110., 31.}, {43., -12.}, {22., 7.}, {91., -18.}};
        for (auto *references : {&mixer_refs, &resistor_refs}) {
            for (auto &reference : *references) {
                reference *= scale;
            }
        }
        auto source_noise = mixer.zero_noise();
        source_noise.covariance(0, 0) = variance;
        const auto mixer_change = renormalize_power_waves(mixer, mixer_refs);
        const auto mixer_boundary = renormalize_conversion_boundary(
            mixer, mixer_refs, {0}, {carrier, {}, {}}, std::vector<Complex>(3), source_noise);
        ConversionDevice changed_mixer{mixer_change.model,
                                       mixer_boundary.source,
                                       mixer_boundary.reflection,
                                       mixer_boundary.source_noise,
                                       mixer.zero_noise()};
        // Physical 25 ohm series resistor at common 50 ohm reference, with its
        // passive thermal noise. Each output frequency has its own local basis.
        const SMatrix resistor{2, {.2, .8, .8, .2}};
        const auto thermal = passive_thermal_noise(resistor, temperature);
        const auto linear =
            lift_linear_conversion(spacing, {2, 18}, {resistor, resistor}, {thermal, thermal});
        const auto resistor_change = renormalize_power_waves(linear.model, resistor_refs);
        const auto resistor_boundary = renormalize_conversion_boundary(linear.model,
                                                                       resistor_refs,
                                                                       {1, 3},
                                                                       linear.source,
                                                                       linear.reflection,
                                                                       linear.source_noise);
        ConversionDevice changed_resistor{
            resistor_change.model,
            resistor_boundary.source,
            resistor_boundary.reflection,
            resistor_boundary.source_noise,
            renormalize_conversion_noise(resistor_change, linear.intrinsic_noise)};
        FrequencyConversionNetwork network(spacing, 75.);
        network.add_power_waves(changed_mixer);
        network.add_power_waves(changed_resistor);
        network.connect(0, 1, 1, 0);
        const auto result = network.analyze(true);
        const auto references = network.reference_impedances_ohms();
        const std::vector<std::size_t> output_indices{4, 6};
        std::vector<Complex> common_a(2), common_b(2), voltage(7), current(7), factors(2);
        for (std::size_t i = 0; i < references.size(); ++i) {
            voltage[i] = (std::conj(references[i]) * result.incident[i] +
                          references[i] * result.outgoing[i]) /
                         std::sqrt(references[i].real());
            current[i] =
                (result.incident[i] - result.outgoing[i]) / std::sqrt(references[i].real());
        }
        for (std::size_t k = 0; k < output_indices.size(); ++k) {
            const auto i = output_indices[k];
            common_a[k] = (voltage[i] + 50. * current[i]) / (2. * std::sqrt(50.));
            common_b[k] = (voltage[i] - 50. * current[i]) / (2. * std::sqrt(50.));
            const auto gamma = changed_resistor.reflection[k * 2 + 1];
            factors[k] = (std::conj(references[i]) * gamma + references[i]) /
                         (std::sqrt(references[i].real()) * std::sqrt(50.));
        }
        SMatrix covariance{2, std::vector<Complex>(4)}, complementary = covariance;
        for (std::size_t i = 0; i < 2; ++i) {
            for (std::size_t j = 0; j < 2; ++j) {
                covariance(i, j) =
                    factors[i] *
                    result.outgoing_noise.covariance(output_indices[i], output_indices[j]) *
                    std::conj(factors[j]);
                complementary(i, j) =
                    factors[i] *
                    result.outgoing_noise.complementary(output_indices[i], output_indices[j]) *
                    factors[j];
            }
        }
        std::cout << std::setprecision(17)
                  << "{\n  \"format\":\"rfmodel.mixed-reference-conversion-example\",\n"
                  << "  \"reference_scale\":" << scale << ",\n"
                  << "  \"parameters\":{\"spacing_hz\":" << spacing
                  << ",\"reference_ohms\":50,\"resistance_ohms\":25,\"temperature_k\":"
                  << temperature << ",\"rf_bin\":8,\"lo_bin\":10,\"phase_radians\":" << phase
                  << ",\"source_noise_w_per_hz\":" << variance << ",\"source\":";
        complex_json(carrier);
        std::cout << "},\n  \"output_bins\":[2,18],\n  \"incident_common\":";
        vector_json(common_a);
        std::cout << ",\n  \"outgoing_common\":";
        vector_json(common_b);
        std::cout << ",\n  \"covariance_common_w_per_hz\":";
        matrix_json(covariance);
        std::cout << ",\n  \"complementary_common_w_per_hz\":";
        matrix_json(complementary);
        std::cout << ",\n  \"reference_impedances_ohms\":";
        vector_json(references);
        std::cout << ",\n  \"incident_local\":";
        vector_json(result.incident);
        std::cout << ",\n  \"outgoing_local\":";
        vector_json(result.outgoing);
        std::cout << ",\n  \"voltage_rms\":";
        vector_json(voltage);
        std::cout << ",\n  \"current_rms\":";
        vector_json(current);
        std::cout << ",\n  \"net_noise_into_device_w_per_hz\":[";
        for (std::size_t i = 0; i < references.size(); ++i) {
            if (i) {
                std::cout << ',';
            }
            std::cout << result.net_noise_into_device_w_per_hz[i];
        }
        std::cout << "],\n  \"relative_residual\":" << result.relative_residual << "\n}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
