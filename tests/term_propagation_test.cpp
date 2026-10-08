#include "rfmodel/term_propagation.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    const MultiToneLimitedAmplifier model(20., 20., 23., 20., 10.);
    const auto input = model.evaluate_terms({1e8, {{10, .001}, {11, .002}}}).terms;
    std::map<double, int> calls;
    auto build = [&](double f) {
        ++calls[f];
        const auto gain = f == 1e8 ? Complex{} : std::polar(.5, -2 * 3.141592653589793 * f * 1e-10);
        LinearNetwork network;
        network.add({2, {0., gain, gain, 0.}});
        return network;
    };
    const auto output = transmit_linear_terms(1e8, input, {0, 1}, 50., build);
    require(output.size() == input.size(), "notches must preserve terms");
    for (std::size_t i = 0; i < input.size(); ++i) {
        require(output[i].contributors == input[i].contributors && output[i].bin == input[i].bin &&
                    output[i].order == input[i].order,
                "linear stage lost provenance");
        const auto gain = input[i].bin == 1
                              ? Complex{}
                              : std::polar(.5, -2 * 3.141592653589793 * input[i].bin * .01);
        near(output[i].amplitude, gain * input[i].amplitude);
    }
    for (const auto &entry : calls) {
        require(entry.second == 1, "evaluate network once per distinct frequency");
    }
    std::vector<AmplifierMixingTerm> cancelling{{3, 10, {-11, 10, 11}, 1.},
                                                {3, 10, {-10, 10, 10}, -1.}};
    const auto preserved = transmit_linear_terms(1e8, cancelling, {0, 3}, 50., [](double) {
        LinearNetwork network;
        network.add({2, {0., .5, .5, .2}});
        network.add({2, {.3, .4, .4, 0.}});
        network.connect(1, 2);
        return network;
    });
    near(preserved[0].amplitude, .2 / .94);
    near(preserved[1].amplitude, -.2 / .94);
    require(preserved.size() == 2, "cancelling identities must not disappear");
    const auto original_calls = calls.size();
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto bad = input;
        if (mutation == 0) {
            bad[0].order = 4;
        }
        if (mutation == 1) {
            bad[0].contributors[0] += 1;
        }
        if (mutation == 2) {
            bad[0].contributors[2] = 1;
        }
        if (mutation == 3) {
            bad.push_back(bad[0]);
        }
        if (mutation == 4) {
            bad[0].amplitude = 1e308;
        }
        rejects<std::invalid_argument>([&] {
            transmit_linear_terms(1e8, bad, {0, 1}, 50., build);
        });
    }
    require(calls.size() == original_calls, "invalid inputs must not invoke builder");
    rejects<std::exception>([&] {
        transmit_linear_terms(1e8, {}, {0, 99}, 50., build);
    });
    rejects<std::exception>([&] {
        transmit_linear_terms(1e8, input, {0, 1}, 75., build);
    });
    rejects<std::overflow_error>([&] {
        transmit_linear_terms(1e8, {{1, 10, {10, 0, 0}, 1e150}}, {0, 1}, 50., [](double) {
            LinearNetwork network;
            network.add({2, {0., 0., 1e100, 0.}});
            return network;
        });
    });
}
