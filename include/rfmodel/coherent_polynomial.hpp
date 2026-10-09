#pragma once
#include "coherence.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <limits>

namespace rfmodel {
struct CoherentPolynomialTerm {
    int order{};
    // Signed one-based indices into the reduced input array; zero padded.
    std::array<int, 9> input_indices{};
    CoherentComponent component;
};

struct CoherentPolynomialResponse {
    std::vector<CoherentComponent> inputs;
    std::vector<CoherentPolynomialTerm> terms;
};

// RF projection of y(t)=sum a[n]*v(t)^n, with explicit local input provenance.
// Harmonic/intermod inputs participate in generation. No implicit P1dB model,
// source-history flattening, DC feedback, or SystemVue coefficient calibration.
class CoherentPolynomial {
    std::vector<double> coefficients_;
    double voltage_scale_;

public:
    explicit CoherentPolynomial(std::vector<double> voltage_coefficients,
                                double reference_ohms = 50.)
        : coefficients_(std::move(voltage_coefficients)) {
        if (coefficients_.empty() || coefficients_.size() > 10 || !std::isfinite(reference_ohms) ||
            reference_ohms <= 0) {
            throw std::invalid_argument("invalid coherent polynomial degree/reference");
        }
        for (double value : coefficients_) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("nonfinite coherent polynomial coefficient");
            }
        }
        if (coefficients_[0] != 0.) {
            throw std::invalid_argument("coherent RF polynomial requires zero constant term");
        }
        voltage_scale_ = std::sqrt(reference_ohms) / std::sqrt(2.);
    }

    CoherentPolynomialResponse evaluate(double spacing_hz,
                                        const std::vector<CoherentComponent> &input,
                                        std::uint64_t reserved_group_max = 0) const {
        CoherentPolynomialResponse result{reduce_coherent_components(spacing_hz, input).components,
                                          {}};
        std::uint64_t highest_group = reserved_group_max;
        std::vector<int> signed_inputs;
        for (std::size_t i = 0; i < result.inputs.size(); ++i) {
            const auto &component = result.inputs[i];
            highest_group = std::max(highest_group, component.coherence_group);
            if (component.amplitude != Complex{}) {
                if (signed_inputs.size() >= 128) {
                    throw std::length_error("coherent polynomial accepts at most 64 active inputs");
                }
                signed_inputs.push_back(-static_cast<int>(i + 1));
                signed_inputs.push_back(static_cast<int>(i + 1));
            }
        }
        std::sort(signed_inputs.begin(), signed_inputs.end());
        std::size_t work = 0;
        for (int order = 1; order < static_cast<int>(coefficients_.size()); ++order) {
            if (coefficients_[order] == 0.) {
                continue;
            }
            double factorial = 1.;
            for (int i = 2; i <= order; ++i) {
                factorial *= i;
            }
            const double coefficient = coefficients_[order] * std::pow(voltage_scale_, order - 1);
            if (!std::isfinite(coefficient)) {
                throw std::overflow_error("coherent polynomial wave coefficient overflow");
            }
            std::array<int, 9> indices{};
            auto append = [&] {
                long long bin = 0;
                double bandwidth = 0.;
                double divisor = 1.;
                int repeat = 0;
                Complex amplitude{coefficient, 0.};
                for (int i = 0; i < order; ++i) {
                    const int index = indices[i];
                    const auto &c = result.inputs[static_cast<std::size_t>(std::abs(index) - 1)];
                    bin += index < 0 ? -static_cast<long long>(c.bin) : c.bin;
                    bandwidth += c.bandwidth_hz;
                    amplitude *= index < 0 ? std::conj(c.amplitude) : c.amplitude;
                    repeat = i > 0 && indices[i] == indices[i - 1] ? repeat + 1 : 1;
                    divisor *= repeat;
                }
                if (std::abs(bin) > std::numeric_limits<int>::max()) {
                    throw std::overflow_error("coherent polynomial frequency overflow");
                }
                if (bin <= 0) {
                    return;
                }
                amplitude *= factorial / divisor;
                if (!std::isfinite(amplitude.real()) || !std::isfinite(amplitude.imag()) ||
                    !std::isfinite(std::norm(amplitude))) {
                    throw std::overflow_error("coherent polynomial amplitude overflow");
                }
                if (amplitude == Complex{}) {
                    return;
                }
                if (result.terms.size() >= 4096) {
                    throw std::length_error("coherent polynomial exceeds 4096 RF terms");
                }
                CoherentComponent component;
                if (order == 1) {
                    component = result.inputs[static_cast<std::size_t>(indices[0] - 1)];
                    component.amplitude = amplitude;
                } else {
                    if (highest_group == std::numeric_limits<std::uint64_t>::max()) {
                        throw std::overflow_error("coherent polynomial group IDs exhausted");
                    }
                    const bool harmonic = indices[0] > 0 && indices[0] == indices[order - 1];
                    component = {static_cast<int>(bin),
                                 harmonic ? SpectrumKind::harmonic : SpectrumKind::intermod,
                                 bandwidth,
                                 ++highest_group,
                                 amplitude};
                }
                result.terms.push_back({order, indices, component});
            };
            std::function<void(int, std::size_t)> enumerate = [&](int depth, std::size_t start) {
                if (++work > 10000000) {
                    throw std::length_error("coherent polynomial enumeration work limit exceeded");
                }
                if (depth == order) {
                    append();
                    return;
                }
                for (std::size_t i = start; i < signed_inputs.size(); ++i) {
                    indices[depth] = signed_inputs[i];
                    enumerate(depth + 1, i);
                }
            };
            enumerate(0, 0);
        }
        std::sort(
            result.terms.begin(), result.terms.end(), [](const auto &left, const auto &right) {
                return std::tie(left.order, left.component.bin, left.input_indices) <
                       std::tie(right.order, right.component.bin, right.input_indices);
            });
        std::vector<CoherentComponent> output;
        for (const auto &term : result.terms) {
            output.push_back(term.component);
        }
        reduce_coherent_components(spacing_hz, output);
        return result;
    }
};
} // namespace rfmodel
