#include "rfmodel/nonlinear_spectrum.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    const MemorylessPolynomial cubic({0., 1., 0., 0.1});
    const auto output = cubic.evaluate({1e6, {{10, .5}, {13, .5}}});
    const auto &spectrum = output.positive_frequency_coefficients;
    near(spectrum.at(10), .6125);
    near(spectrum.at(13), .6125);
    near(spectrum.at(30), .0125);
    near(spectrum.at(39), .0125);
    near(spectrum.at(7), .0375);
    near(spectrum.at(16), .0375);
    near(spectrum.at(33), .0375);
    near(spectrum.at(36), .0375);
    const auto square = MemorylessPolynomial({0., 0., 1.}).evaluate({1., {{0, 2.}, {1, .5}}});
    near(square.positive_frequency_coefficients.at(0), 4.5);
    near(square.positive_frequency_coefficients.at(1), 2.);
    near(square.positive_frequency_coefficients.at(2), .25);
    // Independent time-domain evaluation checks arbitrary complex tone phases.
    const RealVoltageSpectrum phased{1.,
                                     {{0, .2}, {2, std::polar(.3, .7)}, {5, std::polar(.1, -.4)}}};
    const auto transformed = cubic.evaluate(phased);
    auto waveform = [](const RealVoltageSpectrum &data, double theta) {
        double value = 0.;
        for (const auto &entry : data.positive_frequency_coefficients) {
            value += entry.first == 0
                         ? entry.second.real()
                         : 2 * (entry.second * std::polar(1., entry.first * theta)).real();
        }
        return value;
    };
    for (int i = 0; i < 31; ++i) {
        const double theta = i * .17;
        const double x = waveform(phased, theta);
        near(waveform(transformed, theta), x + .1 * x * x * x);
    }
    near(MemorylessPolynomial({3.}).evaluate({1., {}}).positive_frequency_coefficients.at(0), 3.);
    rejects<std::invalid_argument>([&] {
        cubic.evaluate({0., {}});
    });
    rejects<std::invalid_argument>([&] {
        cubic.evaluate({1., {{0, Complex{0., 1.}}}});
    });
    rejects<std::invalid_argument>([&] {
        cubic.evaluate({1., {{-1, 1.}}});
    });
    rejects<std::overflow_error>([&] {
        cubic.evaluate({1., {{std::numeric_limits<int>::max(), 1.}}});
    });
    rejects<std::overflow_error>([&] {
        cubic.evaluate({1., {{1, 1e300}}});
    });
}
