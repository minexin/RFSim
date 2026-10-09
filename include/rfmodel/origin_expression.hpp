#pragma once
#include "polynomial_limits.hpp"
#include "coherence.hpp"
#include "mixing_origin.hpp"
#include <map>

namespace rfmodel {
struct OriginContribution {
    MixingOrigin factors;
    Complex amplitude{};
};

using OriginExpression = std::vector<OriginContribution>;

namespace origin_expression_detail {
struct Less {
    bool operator()(const MixingOrigin &left, const MixingOrigin &right) const {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(), [](const auto &a, const auto &b) {
                return std::tie(a.root_id, a.sign) < std::tie(b.root_id, b.sign);
            });
    }
};

inline void validate_wave(Complex wave) {
    if (!std::isfinite(wave.real()) || !std::isfinite(wave.imag()) ||
        !std::isfinite(std::norm(wave))) {
        throw std::invalid_argument("nonfinite origin-expression wave/power");
    }
}

inline void validate(const std::vector<OriginExpression> &expressions) {
    if (expressions.size() > 4096) {
        throw std::invalid_argument("origin expressions exceed 4096 parents");
    }
    std::size_t terms = 0, factors = 0;
    for (const auto &expression : expressions) {
        if (expression.size() > 4096 || expression.size() > 65536 - terms) {
            throw std::invalid_argument("origin-expression input term limit exceeded");
        }
        terms += expression.size();
        for (const auto &term : expression) {
            if (term.factors.empty() || term.factors.size() > 256 ||
                term.factors.size() > 1048576 - factors) {
                throw std::invalid_argument("origin-expression input factor limit exceeded");
            }
            factors += term.factors.size();
            validate_wave(term.amplitude);
            for (const auto &factor : term.factors) {
                if (factor.root_id == 0 || (factor.sign != 1 && factor.sign != -1)) {
                    throw std::invalid_argument("invalid origin-expression root/sign");
                }
            }
        }
    }
}

class Accumulator {
    std::map<MixingOrigin, coherence_detail::WaveSum, Less> terms_;
    std::size_t factors_{};

public:
    void add(MixingOrigin origin, Complex wave) {
        validate_wave(wave);
        std::sort(origin.begin(), origin.end(), [](const auto &a, const auto &b) {
            return std::tie(a.root_id, a.sign) < std::tie(b.root_id, b.sign);
        });
        auto found = terms_.find(origin);
        if (found == terms_.end()) {
            if (terms_.size() >= 4096 || factors_ + origin.size() > 65536) {
                throw std::length_error("origin-expression output size limit exceeded");
            }
            factors_ += origin.size();
            found = terms_.emplace(std::move(origin), coherence_detail::WaveSum{}).first;
        }
        found->second.real.add(wave.real());
        found->second.imag.add(wave.imag());
    }

    OriginExpression result() const {
        OriginExpression output;
        for (const auto &entry : terms_) {
            const Complex wave{entry.second.real.result(), entry.second.imag.result()};
            validate_wave(wave);
            // Keep exact zero terms: their known source identity is still useful.
            output.push_back({entry.first, wave});
        }
        return output;
    }
};
} // namespace origin_expression_detail

// All terms belong to one physical coherent component. Distinct monomials retain
// separate contributions even when their current amplitudes cancel in total.
inline OriginExpression sum_origin_expressions(const std::vector<OriginExpression> &parents) {
    origin_expression_detail::validate(parents);
    origin_expression_detail::Accumulator accumulator;
    for (const auto &parent : parents) {
        for (const auto &term : parent) {
            accumulator.add(term.factors, term.amplitude);
        }
    }
    return accumulator.result();
}

// Distributive product of 1..11 signed one-based expression indices. Negative
// indices conjugate both the root factors and the actual complex contribution.
// There is no source-order truncation here and no division by a parent wave.
inline OriginExpression product_origin_expressions(const std::vector<OriginExpression> &parents,
                                                   const std::vector<int> &indices,
                                                   Complex coefficient = {1., 0.}) {
    origin_expression_detail::validate(parents);
    origin_expression_detail::validate_wave(coefficient);
    if (indices.empty() || indices.size() > maximum_polynomial_order) {
        throw std::invalid_argument("origin-expression product requires 1..11 indices");
    }
    for (int index : indices) {
        const auto absolute = std::abs(static_cast<long long>(index));
        if (absolute == 0 || static_cast<std::size_t>(absolute) > parents.size()) {
            throw std::invalid_argument("origin-expression index outside parent array");
        }
    }
    // Normalize selected parents before distribution, so duplicate encodings
    // cannot multiply the amount of work without changing the expression.
    std::map<std::size_t, OriginExpression> normalized;
    for (int index : indices) {
        const auto position = static_cast<std::size_t>(std::abs(static_cast<long long>(index)) - 1);
        if (normalized.find(position) == normalized.end()) {
            normalized.emplace(position, sum_origin_expressions({parents[position]}));
        }
    }
    OriginExpression result;
    std::size_t work = 0;
    for (std::size_t step = 0; step < indices.size(); ++step) {
        const int index = indices[step];
        const auto position = static_cast<std::size_t>(std::abs(static_cast<long long>(index)) - 1);
        const auto &parent = normalized.at(position);
        origin_expression_detail::Accumulator next;
        if (step == 0) {
            for (const auto &term : parent) {
                const auto factors = expand_mixing_origin({term.factors}, {index < 0 ? -1 : 1});
                next.add(factors,
                         coefficient * (index < 0 ? std::conj(term.amplitude) : term.amplitude));
            }
        } else {
            for (const auto &left : result) {
                for (const auto &right : parent) {
                    if (++work > 1000000) {
                        throw std::length_error("origin-expression product work limit exceeded");
                    }
                    const auto factors = expand_mixing_origin({left.factors, right.factors},
                                                              {1, index < 0 ? -2 : 2});
                    next.add(factors,
                             left.amplitude *
                                 (index < 0 ? std::conj(right.amplitude) : right.amplitude));
                }
            }
        }
        result = next.result();
    }
    return result;
}

inline Complex origin_expression_amplitude(const OriginExpression &expression) {
    origin_expression_detail::validate({expression});
    coherence_detail::WaveSum sum;
    for (const auto &term : expression) {
        sum.real.add(term.amplitude.real());
        sum.imag.add(term.amplitude.imag());
    }
    const Complex result{sum.real.result(), sum.imag.result()};
    origin_expression_detail::validate_wave(result);
    return result;
}
} // namespace rfmodel
