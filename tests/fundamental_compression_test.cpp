#include "rfmodel/fundamental_compression.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const P1dBFundamentalCompression model(20., 10.);
    const double input = model.input_p1db_watts();
    near(input / std::pow(10., -3.9), 1.);
    const auto phase = std::polar(1., .7);
    const auto output = model.transmit_fundamental(std::sqrt(input) * phase);
    near(std::norm(output) / .01, 1.);
    near(output / std::abs(output), phase);
    near(10. * std::log10(std::norm(output) / input), 19.);
    near(model.transmit_fundamental({}), {});
    near(model.transmit_fundamental(1e-12) / 1e-12, 10.);
    // Equal-frequency waves on distinct physical ports add as powers, not amplitudes.
    const std::vector<PowerWaveSpectrum> ports{
        {1e6, {{10, std::sqrt(input / 4.) * phase}, {20, std::sqrt(input / 4.)}}},
        {1e6, {{10, -std::sqrt(input / 2.) * phase}}}};
    near(incident_rf_power_watts(ports) / input, 1.);
    near(model.transmit_fundamental_from_spectra(ports, 0, 10), .05 * phase);
    near(model.transmit_fundamental_from_spectra(ports, 0, 99), {});
    rejects<std::invalid_argument>([&] {
        model.transmit_fundamental_from_spectra(ports, 2, 10);
    });
    rejects<std::invalid_argument>([&] {
        model.transmit_fundamental_from_spectra(ports, 0, 0);
    });
    rejects<std::invalid_argument>([] {
        incident_rf_power_watts({});
    });
    rejects<std::invalid_argument>([] {
        incident_rf_power_watts({{1., {{0, .1}}}});
    });
    rejects<std::overflow_error>([] {
        incident_rf_power_watts({{1., {{1, 1e308}}}});
    });
    // An upstream native polynomial creates a harmonic; no measured powers are injected.
    const MatchedPolynomialAmplifier cubic("upstream", {0., 1., 0., -100.}, 50.);
    const auto generated = cubic.transmit({1e6, {{10, .005}}});
    const auto fundamental = generated.amplitudes.at(10);
    const auto harmonic = generated.amplitudes.at(30);
    near(fundamental, .0040625);
    near(harmonic, -.0003125);
    const double expected_total = std::norm(fundamental) + std::norm(harmonic);
    near(incident_rf_power_watts({generated}) / expected_total, 1.);
    const auto propagated = model.transmit_fundamental_from_spectra({generated}, 0, 10);
    near(propagated, model.transmit_fundamental(fundamental, expected_total));
    require(std::abs(propagated) < std::abs(model.transmit_fundamental(fundamental)),
            "generated harmonic must contribute compression drive");
    const auto partial_wave = std::sqrt(input / 4.) * phase;
    const auto driven = model.transmit_fundamental(partial_wave, input);
    near(std::norm(driven) / .0025, 1.);
    near(driven / std::abs(driven), phase);
    near(model.transmit_fundamental({}, input), {});
    near(model.transmit_fundamental(partial_wave, std::norm(partial_wave)),
         model.transmit_fundamental(partial_wave));
    for (double invalid : {-1.,
                           input / 8.,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        rejects<std::invalid_argument>([&] {
            model.transmit_fundamental(partial_wave, invalid);
        });
    }
    rejects<std::out_of_range>([&] {
        model.transmit_fundamental(partial_wave, input * 1.01);
    });
    const auto half = model.transmit_fundamental(std::sqrt(input / 2.));
    near(half / (10. * std::sqrt(input / 2.)), (1. + std::pow(10., -.05)) / 2.);
    rejects<std::out_of_range>([&] {
        model.transmit_fundamental(std::sqrt(1.01 * input));
    });
    rejects<std::invalid_argument>([&] {
        model.transmit_fundamental({0., std::numeric_limits<double>::infinity()});
    });
    rejects<std::invalid_argument>([] {
        P1dBFundamentalCompression bad(1e308, 10.);
    });
    rejects<std::invalid_argument>([] {
        P1dBFundamentalCompression bad(20., std::numeric_limits<double>::quiet_NaN());
    });
    // Independently measured low-power chain diagnostic at -50 dBm, no fitted constants.
    Complex wave = std::sqrt(1e-8 / (1. + 77.7 / 290.));
    wave = P1dBFundamentalCompression(25., 60.).transmit_fundamental(wave);
    wave /= std::sqrt(1. + 453.6 / 290.);
    wave = P1dBFundamentalCompression(30., 60.).transmit_fundamental(wave);
    near((std::norm(wave) / 1e-8) / 97266.41556316159, 1.);
}
