#include "rfmodel/coherent_polynomial.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
#include "test_support.hpp"
#include <set>

int main() {
    using namespace rfmodel;
    const std::vector<CoherentComponent> inputs{{10, SpectrumKind::source, 1., 7, {.01, .003}},
                                                {13, SpectrumKind::harmonic, 2., 9, {-.002, .006}}};
    // Compare every RF bin to the independent sparse-convolution implementation,
    // including fifth/ninth order conjugations, multiplicities and phases.
    for (double reference : {50., 75.}) {
        for (int order = 1; order <= 9; ++order) {
            std::vector<double> coefficients(order + 1, 0.);
            coefficients.back() = order % 2 == 0 ? .7 : -.4;
            const auto result =
                CoherentPolynomial(coefficients, reference).evaluate(1e6, inputs, 100);
            const auto expected =
                MatchedPolynomialAmplifier("oracle", coefficients, reference)
                    .transmit({1e6, {{10, inputs[0].amplitude}, {13, inputs[1].amplitude}}});
            std::map<int, Complex> summed;
            std::set<std::uint64_t> groups;
            for (const auto &term : result.terms) {
                require(term.order == order, "homogeneous order");
                require(term.input_indices[order - 1] != 0, "complete provenance");
                for (int i = order; i < 9; ++i) {
                    require(term.input_indices[i] == 0, "zero padding");
                }
                if (order > 1) {
                    require(term.component.coherence_group > 100, "reserved groups");
                    require(groups.insert(term.component.coherence_group).second, "unique origins");
                }
                summed[term.component.bin] += term.component.amplitude;
            }
            std::size_t rf_count = 0;
            for (const auto &entry : expected.amplitudes) {
                if (entry.first > 0) {
                    ++rf_count;
                    require(std::abs(summed.at(entry.first) / entry.second - 1.) < 1e-10,
                            "independent convolution agreement");
                }
            }
            require(rf_count == summed.size(), "complete RF bins");
        }
    }
    // A harmonic and source create new sum/difference IMs; no relabeling is needed.
    const auto remixed = CoherentPolynomial({0., 0., 1.}).evaluate(1e6, inputs);
    bool sum = false, difference = false;
    for (const auto &term : remixed.terms) {
        sum |= term.component.bin == 23 && term.input_indices[0] == 1 && term.input_indices[1] == 2;
        difference |=
            term.component.bin == 3 && term.input_indices[0] == -1 && term.input_indices[1] == 2;
    }
    require(sum && difference, "secondary mixing provenance");
    // Two stages with RF-only projection between them: generated H2 participates
    // in the next stage, checked against independent Fourier convolution.
    const std::vector<CoherentComponent> one{{10, SpectrumKind::source, 1., 7, {.01, .003}}};
    const auto first = CoherentPolynomial({0., 1., .5}).evaluate(1e6, one);
    std::vector<CoherentComponent> next_input;
    for (const auto &term : first.terms) {
        next_input.push_back(term.component);
    }
    const auto second = CoherentPolynomial({0., .8, .4, -.1}).evaluate(1e6, next_input);
    auto first_expected =
        MatchedPolynomialAmplifier("first", {0., 1., .5}).transmit({1e6, {{10, one[0].amplitude}}});
    first_expected.amplitudes.erase(0);
    const auto second_expected =
        MatchedPolynomialAmplifier("second", {0., .8, .4, -.1}).transmit(first_expected);
    std::map<int, Complex> combined;
    for (const auto &term : second.terms) {
        combined[term.component.bin] += term.component.amplitude;
    }
    for (const auto &entry : second_expected.amplitudes) {
        if (entry.first > 0) {
            require(std::abs(combined.at(entry.first) / entry.second - 1.) < 1e-10,
                    "multistage RF projection agrees with convolution");
        }
    }
    auto duplicate = inputs;
    duplicate.push_back(inputs[0]);
    const auto merged = CoherentPolynomial({0., 1.}).evaluate(1e6, duplicate);
    require(merged.inputs.size() == 2, "coherent input reduction");
    near(merged.inputs[0].amplitude, 2. * inputs[0].amplitude);
    duplicate[2].amplitude = -duplicate[0].amplitude;
    const auto cancelled = CoherentPolynomial({0., 0., 1.}).evaluate(1e6, duplicate);
    require(cancelled.inputs.size() == 2 && cancelled.terms.size() == 1,
            "zero groups retained as inputs");
    require(CoherentPolynomial({0.}).evaluate(1e6, {}).terms.empty(), "empty zero model");
    rejects<std::invalid_argument>([] {
        CoherentPolynomial({1., 1.});
    });
    rejects<std::invalid_argument>([] {
        CoherentPolynomial(std::vector<double>(11));
    });
    rejects<std::invalid_argument>([] {
        CoherentPolynomial({0., std::numeric_limits<double>::infinity()});
    });
    rejects<std::overflow_error>([&] {
        CoherentPolynomial({0., 0., 1.}).evaluate(1e6, inputs, UINT64_MAX);
    });
    rejects<std::overflow_error>([] {
        CoherentPolynomial({0., 0., 1.})
            .evaluate(1., {{std::numeric_limits<int>::max(), SpectrumKind::source, 1., 1, 1.}});
    });
    rejects<std::length_error>([] {
        std::vector<CoherentComponent> many;
        for (int i = 0; i < 65; ++i) {
            many.push_back(
                {100 + i, SpectrumKind::source, 1., static_cast<std::uint64_t>(i + 1), .01});
        }
        CoherentPolynomial({0., 1.}).evaluate(1e6, many);
    });
    rejects<std::length_error>([] {
        CoherentPolynomial({0., 0., 0., 0., 0., 0., 0., 0., 0., 1.})
            .evaluate(1e6,
                      {{100, SpectrumKind::source, 1., 1, .01},
                       {101, SpectrumKind::source, 1., 2, .01},
                       {102, SpectrumKind::source, 1., 3, .01},
                       {103, SpectrumKind::source, 1., 4, .01}});
    });
}
