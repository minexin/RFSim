// Independent, deterministic physical anchors for the developer replay tests.
// No SDK includes, probe parser, C ABI, Python, or JSON are used here. Each row is
// name operation payload-token-count payload... expected-token-count expected...
// P1dB is evaluated from its scalar definition. Spectrum products use direct
// signed-Fourier tuple expansion, not the SDK's Horner polynomial evaluator.
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Wide = long double;
using Wave = std::complex<Wide>;
using Spectrum = std::map<int, Wave>;

struct Tone {
    int bin;
    double real;
    double imaginary;
};

std::ostringstream stream() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    return out;
}

void number(std::ostream &out, Wide value) {
    const auto rounded = static_cast<double>(value);
    if (!std::isfinite(rounded)) {
        throw std::runtime_error("oracle result not representable");
    }
    out << rounded;
}

std::string p1db_case(const char *name,
                      double gain,
                      double op1db,
                      double real,
                      double imaginary,
                      unsigned mode,
                      double drive) {
    // Power waves have norm squared in watts. OP1dB denotes the actual,
    // compressed output: IP1dB[dBm] = OP1dB[dBm] - gain[dB] + 1 dB.
    const Wide power = mode == 0 ? Wide(real) * real + Wide(imaginary) * imaginary : drive;
    const Wide input_p1db = std::pow(Wide(10), (Wide(op1db) - gain + 1 - 30) / 10);
    const Wide linear_gain = std::pow(Wide(10), Wide(gain) / 20);
    const Wide compression = 1 - (1 - std::pow(Wide(10), -Wide(1) / 20)) * power / input_p1db;
    auto out = stream();
    out << name << " p1db_fundamental 6 " << gain << ' ' << op1db << ' ' << real << ' ' << imaginary
        << ' ' << mode << ' ' << drive << " 2 ";
    number(out, Wide(real) * linear_gain * compression);
    out << ' ';
    number(out, Wide(imaginary) * linear_gain * compression);
    return out.str();
}

Spectrum polynomial_spectrum(
    double gain, double iip2, double iip3, double reference, const std::vector<Tone> &tones) {
    // v(t)=sum_k V[k] exp(j*k*w*t), V[-k]=conj(V[k]); for RF,
    // V[k]=a[k]*sqrt(R/2). DC uses a different sqrt(R) normalization.
    const Wide resistance = reference;
    const Wide rf_to_voltage = std::sqrt(resistance / 2);
    const Wide a1 = std::pow(Wide(10), Wide(gain) / 20);
    const Wide p2 = std::pow(Wide(10), (Wide(iip2) - 30) / 10);
    const Wide p3 = std::pow(Wide(10), (Wide(iip3) - 30) / 10);
    // Intercepts fix magnitudes; the selected physical convention is positive
    // quadratic and negative cubic. This is an unsaturated polynomial model.
    const Wide a2 = a1 / std::sqrt(2 * resistance * p2);
    const Wide a3 = -2 * a1 / (3 * resistance * p3);
    Spectrum input;
    for (const auto &tone : tones) {
        const Wave voltage = Wave(tone.real, tone.imaginary) * rf_to_voltage;
        if (voltage != Wave{}) {
            input.emplace(tone.bin, voltage);
            input.emplace(-tone.bin, std::conj(voltage));
        }
    }
    Spectrum voltage;
    // Sum each homogeneous order separately, using direct ordered tuples.
    // All multiplicities, collisions, and conjugate phases arise naturally.
    for (const auto &first : input) {
        voltage[first.first] += a1 * first.second;
        for (const auto &second : input) {
            voltage[first.first + second.first] += a2 * first.second * second.second;
            for (const auto &third : input) {
                voltage[first.first + second.first + third.first] +=
                    a3 * first.second * second.second * third.second;
            }
        }
    }
    Spectrum output;
    for (const auto &entry : voltage) {
        if (entry.first < 0 || entry.second == Wave{}) {
            continue;
        }
        const auto scale = entry.first == 0 ? 1 / std::sqrt(resistance) : 1 / rf_to_voltage;
        output.emplace(entry.first,
                       entry.first == 0 ? Wave(entry.second.real() * scale, 0)
                                        : entry.second * scale);
    }
    return output;
}

std::string spectrum_case(const char *name,
                          double spacing,
                          double gain,
                          double iip2,
                          double iip3,
                          double reference,
                          const std::vector<Tone> &tones) {
    const auto expected = polynomial_spectrum(gain, iip2, iip3, reference, tones);
    auto out = stream();
    out << name << " intercept_spectrum " << 6 + 3 * tones.size() << ' ' << spacing << ' ' << gain
        << ' ' << iip2 << ' ' << iip3 << ' ' << reference << ' ' << tones.size();
    for (const auto &tone : tones) {
        out << ' ' << tone.bin << ' ' << tone.real << ' ' << tone.imaginary;
    }
    out << ' ' << 1 + 3 * expected.size() << ' ' << expected.size();
    for (const auto &entry : expected) {
        out << ' ' << entry.first << ' ';
        number(out, entry.second.real());
        out << ' ';
        number(out, entry.second.imag());
    }
    return out.str();
}
} // namespace

int main(int argc, char **) {
    try {
        if (argc != 1) {
            return 2;
        }
        std::vector<std::string> cases;
        cases.push_back(p1db_case("zero-own", 20., 20., 0., 0., 0, 0.));
        cases.push_back(p1db_case("zero-explicit", 20., 20., 0., 0., 1, .001));
        cases.push_back(p1db_case("phase-own", 20., 20., .003, -.004, 0, 0.));
        cases.push_back(p1db_case("phase-same-drive", 20., 20., .003, -.004, 1, .000025));
        cases.push_back(p1db_case("phase-shared-drive", 20., 20., .003, -.004, 1, .00005));
        cases.push_back(p1db_case("explicit-zero-drive", 10., 30., 0., 0., 1, 0.));
        cases.push_back(p1db_case("negative-gain", -6., 12., -.03, .02, 0, 0.));
        cases.push_back(p1db_case("different-compression", 13., 27., .04, .03, 1, .02));
        const auto boundary = std::pow(10., (20. - 20. + 1. - 30.) / 10.);
        cases.push_back(p1db_case("p1db-explicit-boundary", 20., 20., -.01, .005, 1, boundary));
        cases.push_back(p1db_case("p1db-own-boundary", 20., 20., std::sqrt(boundary), 0., 0, 0.));
        cases.push_back(p1db_case("tiny-finite", 20., 20., 1.e-150, -2.e-150, 0, 0.));
        cases.push_back(
            spectrum_case("single-real-50", 1.e6, 20., 20., 10., 50., {{10, .001, 0.}}));
        cases.push_back(
            spectrum_case("single-phase-50", 1.e6, 20., 20., 10., 50., {{10, .003, -.004}}));
        cases.push_back(
            spectrum_case("single-phase-75", 1.e6, 20., 20., 10., 75., {{10, .003, -.004}}));
        cases.push_back(
            spectrum_case("single-phase-1", 1.e6, 20., 20., 10., 1., {{10, .003, -.004}}));
        cases.push_back(spectrum_case(
            "two-equal-50", 1.e6, 20., 20., 10., 50., {{10, .001, 0.}, {13, .001, 0.}}));
        cases.push_back(spectrum_case(
            "two-complex-50", 1.e6, 13., 25., 18., 50., {{13, -.002, .001}, {10, .003, .004}}));
        cases.push_back(spectrum_case(
            "two-complex-75", 1.e6, 13., 25., 18., 75., {{10, .003, .004}, {13, -.002, .001}}));
        cases.push_back(spectrum_case(
            "two-unequal", 2.e6, 20., 20., 10., 50., {{10, .002, -.001}, {13, .0003, .0004}}));
        cases.push_back(spectrum_case(
            "harmonic-collision", 1.e6, 10., 30., 20., 75., {{2, .003, -.004}, {1, .001, .002}}));
        cases.push_back(spectrum_case("single-zero", 1.e6, 20., 20., 10., 50., {{10, 0., 0.}}));
        cases.push_back(
            spectrum_case("two-zero", 1.e6, 20., 20., 10., 50., {{10, 0., 0.}, {13, 0., 0.}}));
        cases.push_back(spectrum_case(
            "one-zero-one-real", 1.e6, 20., 20., 10., 50., {{10, 0., 0.}, {13, .001, 0.}}));
        cases.push_back(
            spectrum_case("maximum-input-bin", 1., 20., 20., 10., 50., {{715827882, .001, 0.}}));
        std::cout.imbue(std::locale::classic());
        std::cout << "RFMODEL_AMPLIFIER_ORACLE 1 " << cases.size() << '\n';
        for (const auto &value : cases) {
            std::cout << value << '\n';
        }
        return std::cout ? 0 : 5;
    } catch (const std::exception &) {
        std::cerr << "amplifier physical oracle failed\n";
        return 5;
    }
}
