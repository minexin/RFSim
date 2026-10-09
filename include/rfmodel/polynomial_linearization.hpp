#pragma once
#include "conversion_operating_point.hpp"
#include "nonlinear_spectrum.hpp"
#include <set>

namespace rfmodel {
// Structural Fourier support of every nonzero polynomial order. Zero-amplitude
// input channels still participate: Newton trials and noise may populate them.
// This is local port-grid planning, not iterative network frequency expansion.
inline std::vector<int> polynomial_output_bins(const std::vector<int> &input_bins,
                                               const MemorylessPolynomial &polynomial) {
    if (input_bins.empty() || input_bins.size() > 512) {
        throw std::invalid_argument("polynomial requires 1..512 input bins");
    }
    std::set<int> signed_bins, unique;
    for (auto bin : input_bins) {
        if (bin < 0 || !unique.insert(bin).second) {
            throw std::invalid_argument("invalid or duplicate polynomial input bin");
        }
        signed_bins.insert(bin);
        signed_bins.insert(-bin);
    }
    std::size_t degree = maximum_polynomial_order;
    while (degree > 0 && polynomial.coefficient(degree) == 0.) {
        --degree;
    }
    std::set<int> output;
    if (polynomial.coefficient(0) != 0.) {
        output.insert(0);
    }
    std::set<int> support{0};
    std::size_t work = 0;
    for (std::size_t order = 1; order <= degree; ++order) {
        std::set<int> next;
        for (auto left : support) {
            for (auto right : signed_bins) {
                if (++work > 10000000) {
                    throw std::length_error("polynomial support work limit exceeded");
                }
                const auto bin = static_cast<long long>(left) + right;
                if (bin > std::numeric_limits<int>::max() ||
                    bin < -static_cast<long long>(std::numeric_limits<int>::max())) {
                    throw std::overflow_error("polynomial support frequency overflow");
                }
                next.insert(static_cast<int>(bin));
                if (next.size() > 4096) {
                    throw std::length_error("polynomial support exceeds 4096 signed bins");
                }
            }
        }
        support = std::move(next);
        if (polynomial.coefficient(order) != 0.) {
            for (auto bin : support) {
                if (bin >= 0) {
                    output.insert(bin);
                }
            }
        }
    }
    return {output.begin(), output.end()};
}

// Matched unilateral y(t)=sum c[n]*v_in(t)^n, including real DC. The output
// port must contain the full structural support; no out-of-band truncation.
// Coefficients are V_out / V_in^n, incident/outgoing are RMS power waves.
inline ConversionLinearization
linearize_polynomial_amplifier(double spacing_hz,
                               const std::vector<ConversionChannel> &channels,
                               const std::vector<Complex> &incident,
                               const MemorylessPolynomial &polynomial,
                               std::size_t input_port = 0,
                               std::size_t output_port = 1,
                               double reference_ohms = 50.) {
    const auto n = channels.size();
    if (!n || n > 512 || incident.size() != n || input_port >= 1024 || output_port >= 1024 ||
        input_port == output_port) {
        throw std::invalid_argument("invalid polynomial linearization parameters");
    }
    auto direct = conversion_detail::zero(n), conjugate = direct;
    FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference_ohms);
    std::map<int, std::size_t> inputs, outputs;
    std::vector<int> input_bins;
    RealVoltageSpectrum voltage{spacing_hz, {}};
    const double root_reference = std::sqrt(reference_ohms), root_two = std::sqrt(2.);
    for (std::size_t i = 0; i < n; ++i) {
        const auto channel = channels[i];
        if ((channel.port != input_port && channel.port != output_port) ||
            !conversion_detail::finite(incident[i]) ||
            (channel.bin == 0 && incident[i].imag() != 0.)) {
            throw std::invalid_argument("invalid polynomial port/wave or complex DC");
        }
        if (channel.port == input_port) {
            inputs.emplace(channel.bin, i);
            input_bins.push_back(channel.bin);
            voltage.positive_frequency_coefficients[channel.bin] =
                incident[i] * (channel.bin ? root_reference / root_two : root_reference);
        } else {
            outputs.emplace(channel.bin, i);
        }
    }
    if (inputs.empty() || outputs.empty()) {
        throw std::invalid_argument("polynomial requires input and output channels");
    }
    for (auto bin : polynomial_output_bins(input_bins, polynomial)) {
        if (!outputs.count(bin)) {
            throw std::invalid_argument("missing polynomial output channel for full support");
        }
    }
    const auto nominal = polynomial.evaluate(voltage);
    const auto derivative = polynomial.derivative().evaluate(voltage);
    auto derivative_at = [&](long long bin) -> Complex {
        const auto positive = std::abs(bin);
        if (positive > std::numeric_limits<int>::max()) {
            return {};
        }
        const auto found =
            derivative.positive_frequency_coefficients.find(static_cast<int>(positive));
        if (found == derivative.positive_frequency_coefficients.end()) {
            return {};
        }
        return bin < 0 ? std::conj(found->second) : found->second;
    };
    std::vector<Complex> outgoing(n);
    for (const auto &output : outputs) {
        const auto row = output.second;
        const double to_wave = (output.first ? root_two : 1.) / root_reference;
        const auto found = nominal.positive_frequency_coefficients.find(output.first);
        if (found != nominal.positive_frequency_coefficients.end()) {
            outgoing[row] = to_wave * found->second;
            if (!conversion_detail::finite(outgoing[row])) {
                throw std::overflow_error("polynomial nominal wave overflow");
            }
        }
        for (const auto &input : inputs) {
            const auto column = input.second;
            // The reference factors cancel because both ports use the same R.
            // Avoid an intermediate sqrt(R)*1/sqrt(R) near range boundaries.
            const double scale = (output.first ? root_two : 1.) / (input.first ? root_two : 1.);
            if (input.first == 0) {
                // A real DC variable has no independent imaginary direction.
                direct(row, column) = conjugate(row, column) =
                    .5 * scale * derivative_at(output.first);
            } else {
                direct(row, column) =
                    scale * derivative_at(static_cast<long long>(output.first) - input.first);
                conjugate(row, column) =
                    scale * derivative_at(static_cast<long long>(output.first) + input.first);
            }
        }
    }
    return {FrequencyConversionModel(spacing_hz, channels, direct, conjugate, reference_ohms),
            outgoing};
}
} // namespace rfmodel
