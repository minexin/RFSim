#include "rfmodel/fundamental_compression.hpp"
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
