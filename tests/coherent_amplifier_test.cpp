#include "rfmodel/coherent_amplifier.hpp"
#include "rfmodel/multitone_amplifier.hpp"
#include "test_support.hpp"
#include <set>

int main() {
    using namespace rfmodel;
    const CoherentLimitedAmplifier model(20., 20., 23., 20., 10.);
    const MultiToneLimitedAmplifier legacy(20., 20., 23., 20., 10.);
    std::vector<CoherentComponent> input{{10, SpectrumKind::source, 1., 7, .01},
                                         {11, SpectrumKind::source, 1., 9, Complex{0., .02}}};
    const auto result = model.evaluate(1e8, input, 100);
    const auto expected = legacy.evaluate_terms({1e8, {{10, .01}, {11, Complex{0., .02}}}});
    require(result.terms.size() == 16, "complete distinct-frequency products");
    near(result.total_input_power_w, expected.total_input_power_w);
    near(result.limited_input_power_w, expected.limited_input_power_w);
    std::set<std::uint64_t> generated;
    for (const auto &term : result.terms) {
        std::array<int, 3> bins{};
        for (int i = 0; i < term.order; ++i) {
            const int index = term.input_indices[i];
            bins[i] = (index < 0 ? -1 : 1) * input[std::abs(index) - 1].bin;
        }
        const auto match =
            std::find_if(expected.terms.begin(), expected.terms.end(), [&](const auto &t) {
                return t.order == term.order && t.contributors == bins;
            });
        require(match != expected.terms.end(), "identity maps to legacy distinct bins");
        near(term.component.amplitude, match->amplitude);
        near(term.component.bandwidth_hz, term.order);
        if (term.order > 1) {
            require(term.component.coherence_group > 100, "reserve bypass groups");
            require(generated.insert(term.component.coherence_group).second, "unique origins");
        }
    }
    input[1].bin = 10;
    const auto same = model.evaluate(1e8, input);
    require(same.terms.size() == 15, "same frequency independent identities retained");
    require(same.inputs.size() == 2, "independent inputs retained");
    near(same.total_input_power_w, .0005);
    int cubic_carriers = 0;
    for (const auto &term : same.terms) {
        if (term.order == 3 && term.component.bin == 10) {
            ++cubic_carriers;
            require(term.component.kind == SpectrumKind::intermod, "cubic carrier is intermod");
        }
    }
    require(cubic_carriers == 6, "do not cancel conjugate input pairs");
    std::reverse(input.begin(), input.end());
    const auto reverse = model.evaluate(1e8, input);
    for (std::size_t i = 0; i < same.terms.size(); ++i) {
        require(same.terms[i].input_indices == reverse.terms[i].input_indices, "stable origins");
        require(same.terms[i].component.coherence_group ==
                    reverse.terms[i].component.coherence_group,
                "stable local group assignment");
        near(same.terms[i].component.amplitude, reverse.terms[i].component.amplitude);
    }
    input = {{10, SpectrumKind::source, 1., 7, .01},
             {10, SpectrumKind::source, 1., 7, Complex{0., .01}}};
    const auto locked = model.evaluate(1e8, input);
    require(locked.inputs.size() == 1 && locked.terms.size() == 4, "merge clocked carriers first");
    const auto locked_expected = legacy.evaluate_terms({1e8, {{10, Complex{.01, .01}}}});
    for (std::size_t i = 0; i < locked.terms.size(); ++i) {
        near(locked.terms[i].component.amplitude, locked_expected.terms[i].amplitude);
    }
    input[1].amplitude = -.01;
    const auto cancelled = model.evaluate(1e8, input, UINT64_MAX);
    require(cancelled.terms.size() == 1, "preserve zero direct group only");
    near(cancelled.total_input_power_w, 0.);
    near(cancelled.terms[0].component.amplitude, 0.);
    input[1].bandwidth_hz = 2.;
    require(model.evaluate(1e8, input).inputs.size() == 2, "bandwidth is part of identity");
    require(model.evaluate(1e8, {}).terms.empty(), "empty input");
    // Conducted distortion contributes to drive but is not a fresh source.
    const double anchor = std::pow(10., -2.9);
    const double half_wave = std::sqrt(anchor / 2);
    const auto cascade =
        model.evaluate_cascade(1e8,
                               {{10, SpectrumKind::source, 1., 7, half_wave},
                                {20, SpectrumKind::harmonic, 2., 8, Complex{0., half_wave}}});
    near(cascade.total_input_power_w, anchor);
    require(cascade.terms.size() == 5, "two conducted terms and three carrier products");
    near(cascade.terms[0].component.amplitude, std::sqrt(.05));
    near(cascade.terms[1].component.amplitude, Complex{0., std::sqrt(.05)});
    require(cascade.terms[1].component.kind == SpectrumKind::harmonic &&
                cascade.terms[1].component.coherence_group == 8,
            "conducted distortion retains kind and group");
    for (const auto &term : cascade.terms) {
        if (term.order > 1) {
            for (int i = 0; i < term.order; ++i) {
                require(std::abs(term.input_indices[i]) == 1,
                        "secondary distortion does not generate new mixing products");
            }
        }
    }
    const auto distortion_only =
        model.evaluate_cascade(1e8, {{20, SpectrumKind::harmonic, 2., 8, Complex{0., half_wave}}});
    require(distortion_only.terms.size() == 1, "no carrier, no new products");
    require(distortion_only.terms[0].component.kind == SpectrumKind::harmonic,
            "do not relabel a harmonic as a carrier");

    rejects<std::overflow_error>([&] {
        model.evaluate(1e8, input, UINT64_MAX);
    });
    input[1].kind = SpectrumKind::harmonic;
    rejects<std::invalid_argument>([&] {
        model.evaluate(1e8, input);
    });
    rejects<std::invalid_argument>([&] {
        model.evaluate(0., {});
    });
    rejects<std::invalid_argument>([&] {
        model.evaluate(
            1e8, {{1, SpectrumKind::source, 1., 1, .01}, {2, SpectrumKind::source, 3e8, 2, .01}});
    });
    rejects<std::overflow_error>([&] {
        model.evaluate(1., {{INT32_MAX, SpectrumKind::source, 1., 1, .01}});
    });
    std::vector<CoherentComponent> many;
    for (int i = 1; i <= 65; ++i) {
        many.push_back({10, SpectrumKind::source, 1., static_cast<std::uint64_t>(i), .001});
    }
    rejects<std::length_error>([&] {
        model.evaluate(1e8, many);
    });
    many.resize(20);
    rejects<std::length_error>([&] {
        model.evaluate(1e8, many);
    });
    rejects<std::invalid_argument>([&] {
        model.evaluate(1e8, std::vector<CoherentComponent>(4097));
    });
}
