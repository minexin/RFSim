#include "rfmodel/coherent_highorder_amplifier.hpp"
#include "rfmodel/polynomial_intercepts.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    const auto coefficients =
        polynomial_coefficients_from_intercepts(10., {{1, 1, 30., 1}, {2, -1, 20., -1}});
    const std::vector<double> nonlinear(coefficients.begin() + 2, coefficients.end());
    const CoherentHighOrderAmplifier model(10., 20., 23., nonlinear);
    const CoherentLimitedAmplifier legacy(10., 20., 23., 30., 20.);
    for (double magnitude : {0., .001, .05, .2, 1e100}) {
        const std::vector<CoherentComponent> input{
            {10, SpectrumKind::source, 1., 7, std::polar(magnitude, .3)},
            {13, SpectrumKind::source, 1., 9, std::polar(magnitude * .8, -.4)}};
        const auto actual = model.evaluate(1e8, input, 100);
        const auto expected = legacy.evaluate(1e8, input, 100);
        require(actual.inputs.size() == expected.inputs.size(), "same reduced inputs");
        require(actual.terms.size() == expected.terms.size(), "same cubic term count");
        near(actual.total_input_power_w, expected.total_input_power_w);
        near(actual.operating_point.limited_input_power_w, expected.limited_input_power_w);
        for (std::size_t i = 0; i < actual.terms.size(); ++i) {
            const auto &a = actual.terms[i];
            const auto &b = expected.terms[i];
            require(a.order == b.order && a.component.bin == b.component.bin &&
                        a.component.kind == b.component.kind,
                    "legacy cubic identity parity");
            require(a.order == 1 ? a.component.coherence_group == b.component.coherence_group
                                 : a.component.coherence_group > 100,
                    "direct groups persist; generated IDs are local");
            for (int j = 0; j < 3; ++j) {
                require(a.input_indices[j] == b.input_indices[j], "legacy indices");
            }
            near(a.component.amplitude, b.component.amplitude);
        }
    }
    for (int order : {4, 7, 9, 11}) {
        std::vector<double> a(order - 1, 0.);
        a.back() = -.02;
        const CoherentHighOrderAmplifier high(10., 20., 23., a, 75.);
        const std::vector<CoherentComponent> inputs{{10, SpectrumKind::source, 1., 7, {.06, .02}},
                                                    {13, SpectrumKind::source, 1., 9, {.05, -.03}}};
        const auto response = high.evaluate(1e8, inputs);
        require(response.operating_point.nonlinear_input_scale < 1., "common input limit active");
        std::vector<double> full(order + 1, 0.);
        full.back() = -.02;
        std::map<int, Complex> waves;
        for (const auto &c : inputs) {
            waves[c.bin] = c.amplitude * response.operating_point.nonlinear_input_scale;
        }
        const auto oracle =
            MatchedPolynomialAmplifier("independent convolution", full, 75.).transmit({1e8, waves});
        std::map<int, Complex> generated;
        for (const auto &term : response.terms) {
            if (term.order == 1) {
                const auto &parent = response.inputs[term.input_indices[0] - 1];
                near(term.component.amplitude,
                     parent.amplitude * response.operating_point.fundamental_amplitude_gain);
            } else {
                generated[term.component.bin] += term.component.amplitude;
                require(term.input_indices[order - 1] != 0, "complete high-order provenance");
            }
        }
        for (const auto &entry : oracle.amplitudes) {
            if (entry.first > 0) {
                require(std::abs(generated.at(entry.first) / entry.second - 1.) < 1e-10,
                        "high-order convolution with common limiting");
            }
        }
    }
    const std::vector<CoherentComponent> cancellation{{10, SpectrumKind::source, 1., 7, .01},
                                                      {10, SpectrumKind::source, 1., 7, -.01}};
    const auto zero = model.evaluate(1e8, cancellation);
    require(zero.terms.size() == 1 && zero.terms[0].component.amplitude == Complex{} &&
                zero.operating_point.nonlinear_input_scale == 1.,
            "cancelled source keeps its zero direct group");
    const std::vector<CoherentComponent> distortion{{10, SpectrumKind::harmonic, 1., 7, .03},
                                                    {13, SpectrumKind::source, 1., 9, .04}};
    rejects<std::invalid_argument>([&] {
        model.evaluate(1e8, distortion);
    });
    const auto cascade = model.evaluate(1e8, distortion, 100, true);
    near(cascade.total_input_power_w, .0025);
    for (const auto &term : cascade.terms) {
        if (term.order > 1) {
            for (int i = 0; i < term.order; ++i) {
                require(std::abs(term.input_indices[i]) == 2, "distortion only propagates");
            }
        }
    }
    const CoherentHighOrderAmplifier direct(10., 20., 23., {});
    require(direct.evaluate(1e8, distortion, UINT64_MAX, true).terms.size() == 2,
            "no nonlinear coefficients require no new group IDs");
    rejects<std::invalid_argument>([] {
        CoherentHighOrderAmplifier(10., 20., 23., std::vector<double>(11));
    });
    rejects<std::invalid_argument>([] {
        CoherentHighOrderAmplifier(10., 20., 23., {std::numeric_limits<double>::quiet_NaN()});
    });
    rejects<std::invalid_argument>([&] {
        model.operating_point(-1.);
    });
    rejects<std::overflow_error>([&] {
        model.evaluate(1e8, {{10, SpectrumKind::source, 1., 1, .01}}, UINT64_MAX);
    });
    std::vector<CoherentComponent> many;
    for (int i = 0; i < 4096; ++i) {
        many.push_back({100 + i, SpectrumKind::source, 1., static_cast<std::uint64_t>(i + 1), 0.});
    }
    many[0].amplitude = .01;
    require(direct.evaluate(1e6, many).terms.size() == 4096, "direct-only input boundary");
    rejects<std::length_error>([&] {
        model.evaluate(1e6, many);
    });
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
