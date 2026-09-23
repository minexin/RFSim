#include "rfmodel/spectrum_analysis.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
#include "rfmodel/ideal_mixer.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    const auto amplifier = MatchedPolynomialAmplifier::from_iip3("amplifier", 20., 10.);
    const IdealRealMixer mixer("mixer", 90, 0.);
    const PowerWaveSpectrum source{1e6, {{100, std::sqrt(1e-5)}, {103, std::sqrt(1e-5)}}};
    const std::vector<SpectrumStage> stages{
        {"amplifier",
         50.,
         50.,
         [&](const PowerWaveSpectrum &in) {
             return amplifier.transmit(in);
         }},
        {"mixer",
         50.,
         50.,
         [&](const PowerWaveSpectrum &in) {
             return mixer.transmit(in);
         }},
        {"IF filter", 50., 50., [&](const PowerWaveSpectrum &in) {
             return transmit_linear_spectrum(in, {0, 1}, 50., [](double f) {
                 const double transmission = f <= 30e6 ? 1. : 0.;
                 LinearNetwork net;
                 net.add(SMatrix{2, {0., transmission, transmission, 0.}});
                 return net;
             });
         }}};
    const auto result = analyze_spectrum(source, 50., stages);
    near(result.input_power_watts, 2e-5);
    require(result.stages.size() == 3 && result.stages[2].name == "IF filter", "stage snapshots");
    const auto &output = result.stages.back().output.amplitudes;
    require(output.size() == 4, "two fundamentals and two IM3 products");
    near(output.at(10), 10 * std::sqrt(1e-5) * (1 - 3 * 1e-5 / .01));
    near(output.at(13), output.at(10));
    near(output.at(7), -10 * std::sqrt(1e-5) * (1e-5 / .01));
    near(output.at(16), output.at(7));
    near(result.stages.back().total_power_watts,
         2 * (std::norm(output.at(10)) + std::norm(output.at(7))));
    int calls = 0;
    auto bad = stages;
    bad[0].evaluate = [&](const PowerWaveSpectrum &in) {
        ++calls;
        return in;
    };
    bad[2].input_reference_ohms = 75.;
    rejects<std::invalid_argument>([&] {
        analyze_spectrum(source, 50., bad);
    });
    require(calls == 0, "contracts validated before execution");
    try {
        analyze_spectrum(source, 50., {{"broken", 50., 50., [](const PowerWaveSpectrum &in) {
                                            auto changed = in;
                                            changed.spacing_hz *= 2;
                                            return changed;
                                        }}});
        require(false, "grid mismatch accepted");
    } catch (const std::runtime_error &error) {
        require(std::string(error.what()).find("broken") != std::string::npos,
                "stage error context");
    }
    rejects<std::runtime_error>([] {
        transmit_linear_spectrum({1., {{0, 1.}}}, {0, 1}, 50., [](double) {
            LinearNetwork net;
            net.add(SMatrix{2, {0., 0., Complex{0., 1.}, 0.}});
            return net;
        });
    });
}
