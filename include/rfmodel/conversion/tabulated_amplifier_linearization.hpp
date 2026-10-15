#pragma once
#include "rfmodel/conversion/conversion_operating_point.hpp"
#include "rfmodel/nonlinear/tabulated_amplifier.hpp"

namespace rfmodel {
// Matched unilateral, common input-RF radius. No output-incident drive, added
// frequencies, intrinsic noise, or nonzero DC. Returns F, not an affine offset.
inline ConversionLinearization
linearize_tabulated_amplifier(double spacing_hz,
                              const std::vector<ConversionChannel> &channels,
                              const std::vector<Complex> &incident,
                              const TabulatedAmplifier &amplifier,
                              std::size_t input_port = 0,
                              std::size_t output_port = 1,
                              double reference_ohms = 50.) {
    const auto n = channels.size();
    if (!n || n > 512 || incident.size() != n || input_port >= 1024 || output_port >= 1024 ||
        input_port == output_port) {
        throw std::invalid_argument("invalid tabulated amplifier linearization dimensions");
    }
    auto direct = conversion_detail::zero(n), conjugate = direct;
    FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference_ohms);
    std::map<int, std::size_t> inputs, outputs;
    double radius = 0.;
    bool rf = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto channel = channels[i];
        if ((channel.port != input_port && channel.port != output_port) ||
            !conversion_detail::finite(incident[i]) ||
            (channel.bin == 0 && incident[i] != Complex{})) {
            throw std::invalid_argument("invalid tabulated amplifier wave, port, or nonzero DC");
        }
        (channel.port == input_port ? inputs : outputs).emplace(channel.bin, i);
        if (channel.port == input_port && channel.bin > 0) {
            rf = true;
            radius = std::hypot(radius, std::hypot(incident[i].real(), incident[i].imag()));
        }
    }
    if (!std::isfinite(radius)) {
        throw std::overflow_error("tabulated amplifier total radius overflow");
    }
    if (!rf || inputs.size() != outputs.size()) {
        throw std::invalid_argument("tabulated amplifier requires matching RF port bins");
    }
    const auto response = amplifier.response(radius);
    std::vector<Complex> outgoing(n);
    for (const auto &entry : inputs) {
        const auto found = outputs.find(entry.first);
        if (found == outputs.end()) {
            throw std::invalid_argument("tabulated amplifier input/output frequency grids differ");
        }
        if (entry.first == 0) {
            continue;
        }
        const auto input = entry.second, output = found->second;
        outgoing[output] = amplifier.transmit(incident[input], radius);
        direct(output, input) = response.gain;
        if (radius > 0.) {
            auto coupling = [&](Complex column, bool conjugated) {
                using tabulated_amplifier_detail::Wide;
                const std::array<Wide, 2> q{response.radial_gain_difference.real(),
                                            response.radial_gain_difference.imag()};
                const std::array<Wide, 2> a{incident[input].real(), incident[input].imag()};
                const std::array<Wide, 2> b{column.real(),
                                            conjugated ? -column.imag() : column.imag()};
                Wide real = 0, imaginary = 0;
                for (unsigned i = 0; i < 2; ++i) {
                    for (unsigned j = 0; j < 2; ++j) {
                        for (unsigned k = 0; k < 2; ++k) {
                            const Wide value = tabulated_amplifier_detail::scaled(
                                {.5L, q[i], a[j], b[k]}, {radius, radius});
                            switch ((i + j + k) % 4) {
                            case 0:
                                real += value;
                                break;
                            case 1:
                                imaginary += value;
                                break;
                            case 2:
                                real -= value;
                                break;
                            case 3:
                                imaginary -= value;
                                break;
                            }
                        }
                    }
                }
                const Complex result{static_cast<double>(real), static_cast<double>(imaginary)};
                if (!conversion_detail::finite(result)) {
                    throw std::overflow_error("tabulated amplifier coupling overflow");
                }
                return result;
            };
            for (const auto &column : inputs) {
                if (column.first == 0) {
                    continue;
                }
                direct(output, column.second) += coupling(incident[column.second], true);
                conjugate(output, column.second) += coupling(incident[column.second], false);
            }
        }
    }
    return {FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference_ohms),
            outgoing};
}

// Own table and coordinate data, so temporaries and later caller mutation are safe.
inline ConversionNonlinearDevice
make_tabulated_amplifier_device(std::size_t device,
                                double spacing_hz,
                                std::vector<ConversionChannel> channels,
                                TabulatedAmplifier amplifier,
                                std::size_t input_port = 0,
                                std::size_t output_port = 1,
                                double reference_ohms = 50.) {
    linearize_tabulated_amplifier(spacing_hz,
                                  channels,
                                  std::vector<Complex>(channels.size()),
                                  amplifier,
                                  input_port,
                                  output_port,
                                  reference_ohms);
    return {
        device,
        [spacing_hz,
         channels = std::move(channels),
         amplifier = std::move(amplifier),
         input_port,
         output_port,
         reference_ohms](const std::vector<Complex> &incident) {
            return linearize_tabulated_amplifier(
                spacing_hz, channels, incident, amplifier, input_port, output_port, reference_ohms);
        }};
}
} // namespace rfmodel
