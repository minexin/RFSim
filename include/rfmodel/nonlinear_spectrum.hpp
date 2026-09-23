#pragma once
#include "device_model.hpp"
#include <cmath>
#include <limits>
#include <map>

namespace rfmodel {
// v(t)=V[0]+2*Re(sum_{k>0} V[k]*exp(j*2*pi*k*spacing_hz*t)).
// Coefficients are Fourier-series voltages, not RMS or power waves.
struct RealVoltageSpectrum {
    double spacing_hz{};
    std::map<int, Complex> positive_frequency_coefficients;
};

class MemorylessPolynomial {
    std::vector<double> coefficients_;

public:
    // y(t)=sum_n coefficients[n]*v(t)^n. Units: V_out / V_in^n.
    explicit MemorylessPolynomial(std::vector<double> coefficients)
        : coefficients_(std::move(coefficients)) {
        if (coefficients_.empty() || coefficients_.size() > 10) {
            throw std::invalid_argument("polynomial requires degree zero through nine");
        }
        for (double coefficient : coefficients_) {
            if (!std::isfinite(coefficient)) {
                throw std::invalid_argument("nonfinite polynomial coefficient");
            }
        }
        while (coefficients_.size() > 1 && coefficients_.back() == 0.) {
            coefficients_.pop_back();
        }
    }

    RealVoltageSpectrum evaluate(const RealVoltageSpectrum &input) const {
        if (!std::isfinite(input.spacing_hz) || input.spacing_hz <= 0 ||
            input.positive_frequency_coefficients.size() > 2048) {
            throw std::invalid_argument("invalid spectrum spacing or size");
        }
        std::map<int, Complex> signed_input;
        for (const auto &entry : input.positive_frequency_coefficients) {
            const int bin = entry.first;
            const auto value = entry.second;
            if (bin < 0 || !std::isfinite(value.real()) || !std::isfinite(value.imag()) ||
                !std::isfinite(bin * input.spacing_hz) || (bin == 0 && value.imag() != 0.)) {
                throw std::invalid_argument("invalid real voltage spectrum coefficient");
            }
            if (value == Complex{}) {
                continue;
            }
            signed_input[bin] = value;
            if (bin > 0) {
                signed_input[-bin] = std::conj(value);
            }
        }
        // Horner evaluation using sparse convolution on signed integer bins.
        // No wraparound or out-of-band truncation is permitted.
        std::map<int, Complex> result{{0, coefficients_.back()}};
        std::size_t products = 0;
        for (std::size_t degree = coefficients_.size() - 1; degree > 0; --degree) {
            std::map<int, Complex> next;
            for (const auto &left : result) {
                if (left.second == Complex{}) {
                    continue;
                }
                for (const auto &right : signed_input) {
                    if (++products > 10000000) {
                        throw std::length_error("polynomial convolution work limit exceeded");
                    }
                    const auto bin = static_cast<long long>(left.first) + right.first;
                    if (bin > std::numeric_limits<int>::max() ||
                        bin < -static_cast<long long>(std::numeric_limits<int>::max())) {
                        throw std::overflow_error("nonlinear frequency index overflow");
                    }
                    auto &value = next[static_cast<int>(bin)];
                    value += left.second * right.second;
                    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                        throw std::overflow_error("nonlinear voltage overflow");
                    }
                    if (next.size() > 4096) {
                        throw std::length_error("nonlinear spectrum bin limit exceeded");
                    }
                }
            }
            next[0] += coefficients_[degree - 1];
            result = std::move(next);
        }
        RealVoltageSpectrum output{input.spacing_hz, {}};
        for (const auto &entry : result) {
            if (!std::isfinite(entry.second.real()) || !std::isfinite(entry.second.imag()) ||
                !std::isfinite(entry.first * input.spacing_hz)) {
                throw std::overflow_error("nonlinear result overflow");
            }
            if (entry.first >= 0 && entry.second != Complex{}) {
                // DC is real analytically; discard only its roundoff imaginary part.
                output.positive_frequency_coefficients[entry.first] =
                    entry.first == 0 ? Complex{entry.second.real(), 0.} : entry.second;
            }
        }
        return output;
    }
};
} // namespace rfmodel
