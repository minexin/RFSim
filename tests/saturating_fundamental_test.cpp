#include "rfmodel/saturating_fundamental.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const SaturatingFundamentalCompression model(20., 20., 23.);
    const P1dBFundamentalCompression low(20., 20.);
    const double p1 = model.input_p1db_watts();
    const double limit = std::pow(10., -.7);
    const auto phase = std::polar(1., .7);
    for (double scale : {0., 1e-9, .1, .9, 1.}) {
        const auto incident = std::sqrt(p1 * scale) * phase;
        near(model.transmit_fundamental(incident), low.transmit_fundamental(incident));
    }
    near(std::norm(model.transmit_fundamental(std::sqrt(p1))) / .1, 1.);
    near(model.transmit_fundamental({}), {});
    // Test derivative from each side independently at the amplitude anchor.
    const double x = std::sqrt(p1), step = x * 1e-6;
    const double anchor = std::abs(model.transmit_fundamental(x));
    const double left = (anchor - std::abs(model.transmit_fundamental(x - step))) / step;
    const double right = (std::abs(model.transmit_fundamental(x + step)) - anchor) / step;
    const double slope = 10. * (3. * std::pow(10., -.05) - 2.);
    require(std::abs(left / slope - 1.) < 1e-6 && std::abs(right / slope - 1.) < 1e-6,
            "P1dB amplitude slope must be continuous");
    double previous = .1;
    for (double power : {.002, .004, .01, .1, 100., 1e300}) {
        const auto wave = model.transmit_fundamental(std::sqrt(power) * phase);
        const double output_power = std::norm(wave);
        require(output_power >= previous - 1e-14 && output_power <= limit * (1. + 1e-14),
                "saturation must be monotone and bounded");
        near(wave / std::abs(wave), phase);
        previous = output_power;
        near(model.transmit_fundamental(std::sqrt(power) * phase / 2., power), wave / 2.);
    }
    near(previous / limit, 1.);
    near(model.transmit_fundamental({}, 1.), {});
    for (double saturation : {20., 19., 1e308, std::numeric_limits<double>::quiet_NaN()}) {
        rejects<std::invalid_argument>([&] {
            SaturatingFundamentalCompression invalid(20., 20., saturation);
        });
    }
    for (double total : {-1., .005, std::numeric_limits<double>::infinity()}) {
        rejects<std::invalid_argument>([&] {
            model.transmit_fundamental(.1, total);
        });
    }
    rejects<std::invalid_argument>([&] {
        model.transmit_fundamental({1e308, 0.});
    });
    // The older API deliberately retains its below-P1dB contract.
    rejects<std::out_of_range>([&] {
        low.transmit_fundamental(.1);
    });
}
