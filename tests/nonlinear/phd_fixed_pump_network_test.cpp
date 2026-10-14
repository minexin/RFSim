#include "rfmodel/nonlinear/phd_fixed_pump_network.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <type_traits>

namespace {
using namespace rfmodel;
using Waves = std::vector<Complex>;
std::size_t checks = 0;

void require(bool condition, const std::string &message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void close(Complex actual, Complex expected, const std::string &message, double tolerance = 2e-11) {
    const double error = std::abs(actual - expected);
    require(std::isfinite(error) && error <= tolerance * (1. + std::abs(expected)), message);
}

template <class Exception, class Function>
void rejects(Function action, const std::string &message) {
    ++checks;
    try {
        action();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error("expected rejection: " + message);
}

SMatrix zero(std::size_t n) {
    return {n, Waves(n * n)};
}

Complex power(Complex value, int exponent) {
    if (exponent < 0) {
        value = 1. / value;
        exponent = -exponent;
    }
    Complex result = 1.;
    for (int i = 0; i < exponent; ++i) {
        result *= value;
    }
    return result;
}

// Independent closed real 2x2 solution of alpha*a + beta*conj(a) = rhs.
Complex scalar_oracle(Complex alpha, Complex beta, Complex rhs) {
    const double xx = (alpha + beta).real(), xy = -(alpha - beta).imag();
    const double yx = (alpha + beta).imag(), yy = (alpha - beta).real();
    const double det = xx * yy - xy * yx;
    return {(rhs.real() * yy - xy * rhs.imag()) / det, (xx * rhs.imag() - yx * rhs.real()) / det};
}

PhdCoefficientData fixture() {
    PhdCoefficientData d;
    d.fundamental_hz = 1e9;
    d.port_reference_ohms = {50.};
    d.port_reference_planes = {"native"};
    d.channels = {{0, 1}, {0, 2}, {0, 3}};
    d.pump_channel = 0;
    d.pump_magnitude_sqrt_w = .8;
    d.baseline_outgoing = {{.2, .1}, {.4, -.3}, {-.1, .2}};
    d.direct = zero(3);
    d.conjugate = zero(3);
    d.direct(1, 1) = {.23, .11};
    d.conjugate(1, 1) = {-.17, .09};
    d.direct(2, 2) = {.12, -.08};
    d.conjugate(2, 2) = {.04, .03};
    return d;
}

using LongComplex = std::complex<long double>;
using Spectrum = std::map<int, LongComplex>;
constexpr std::size_t harmonics = 6;

// Independent physical oracle: x(t) = a0 + sqrt(2)*Re sum(a_h exp(i*h*t)),
// y(t) = c*x(t)^3. Fourier coefficients are a_h/sqrt(2), with their negative
// frequency conjugates. An ordered triple convolution retains multiplicity;
// DC is projected without sqrt(2), positive outputs with sqrt(2).
Waves cubic_fourier(const Waves &incident, long double coefficient) {
    const long double root_two = std::sqrt(2.L);
    Spectrum x;
    x[0] = incident[0].real();
    for (std::size_t h = 1; h < harmonics; ++h) {
        const LongComplex value{incident[h].real(), incident[h].imag()};
        x[static_cast<int>(h)] = value / root_two;
        x[-static_cast<int>(h)] = std::conj(value) / root_two;
    }
    Spectrum y;
    for (const auto &a : x) {
        for (const auto &b : x) {
            for (const auto &c : x) {
                y[a.first + b.first + c.first] += coefficient * a.second * b.second * c.second;
            }
        }
    }
    Waves outgoing(2 * harmonics);
    for (std::size_t h = 0; h < harmonics; ++h) {
        const auto wave = y[static_cast<int>(h)] * (h == 0 ? 1.L : root_two);
        outgoing[harmonics + h] = {static_cast<double>(wave.real()),
                                   static_cast<double>(wave.imag())};
    }
    return outgoing;
}

PhdCoefficientData cubic_fixture(double coefficient, double radius) {
    PhdCoefficientData data;
    data.fundamental_hz = 1e8;
    data.port_reference_ohms = {50., 50.};
    data.port_reference_planes = {"cubic:input", "cubic:output"};
    for (std::size_t port = 0; port < 2; ++port) {
        for (std::size_t h = 0; h < harmonics; ++h) {
            data.channels.push_back({port, static_cast<int>(h)});
        }
    }
    data.pump_channel = 1;
    data.pump_magnitude_sqrt_w = radius;
    data.baseline_outgoing = Waves(2 * harmonics);
    // cos^3(t) = (3*cos(t) + cos(3*t))/4, in RMS coordinates.
    data.baseline_outgoing[harmonics + 1] = 1.5 * coefficient * radius * radius * radius;
    data.baseline_outgoing[harmonics + 3] = .5 * coefficient * radius * radius * radius;
    data.direct = zero(2 * harmonics);
    data.conjugate = zero(2 * harmonics);
    // d(c*x^3) = 3*c*x^2*dx. At a real pump, x^2 has coefficients
    // Q[0]=r^2 and Q[+/-2]=r^2/2. This analytic construction is separate
    // from the ordered cubic-convolution finite-difference oracle above.
    const auto square_coefficient = [radius](int h) {
        return h == 0 ? radius * radius : (std::abs(h) == 2 ? .5 * radius * radius : 0.);
    };
    for (std::size_t m = 0; m < harmonics; ++m) {
        for (std::size_t n = 0; n < harmonics; ++n) {
            if (n == data.pump_channel) {
                continue; // No F/r surrogate or pump-amplitude derivative exists.
            }
            const auto row = harmonics + m;
            if (n == 0) {
                const double derivative = 3. * coefficient *
                                          square_coefficient(static_cast<int>(m)) *
                                          (m == 0 ? 1. : std::sqrt(2.));
                data.direct(row, n) = data.conjugate(row, n) = .5 * derivative;
            } else {
                const double scale = 3. * coefficient / (m == 0 ? std::sqrt(2.) : 1.);
                data.direct(row, n) =
                    scale * square_coefficient(static_cast<int>(m) - static_cast<int>(n));
                data.conjugate(row, n) = scale * square_coefficient(static_cast<int>(m + n));
            }
        }
    }
    data.provenance = "analytic real cubic in native RMS wave coordinates";
    return data;
}

// Four-real-dimensional Cramer's-rule oracle. Explicit determinant permutation
// expansion is independent of the network's elimination/factorization routine.
double determinant4(const std::array<std::array<double, 4>, 4> &m) {
    std::array<int, 4> p{{0, 1, 2, 3}};
    double result = 0.;
    do {
        int inversions = 0;
        double term = 1.;
        for (int i = 0; i < 4; ++i) {
            term *= m[static_cast<std::size_t>(i)][static_cast<std::size_t>(p[i])];
            for (int j = i + 1; j < 4; ++j) {
                if (p[i] > p[j]) {
                    ++inversions;
                }
            }
        }
        result += (inversions % 2 ? -term : term);
    } while (std::next_permutation(p.begin(), p.end()));
    return result;
}

std::array<Complex, 2> pair_oracle(const PhdCoefficientData &d,
                                   const std::array<Complex, 2> &gamma,
                                   const std::array<Complex, 2> &source) {
    std::array<std::array<double, 4>, 4> m{};
    std::array<double, 4> rhs{};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto r = source[i] + gamma[i] * d.baseline_outgoing[i + 1];
        rhs[2 * i] = r.real();
        rhs[2 * i + 1] = r.imag();
        for (std::size_t j = 0; j < 2; ++j) {
            const Complex alpha = (i == j ? 1. : 0.) - gamma[i] * d.direct(i + 1, j + 1);
            const Complex beta = -gamma[i] * d.conjugate(i + 1, j + 1);
            m[2 * i][2 * j] = (alpha + beta).real();
            m[2 * i][2 * j + 1] = -(alpha - beta).imag();
            m[2 * i + 1][2 * j] = (alpha + beta).imag();
            m[2 * i + 1][2 * j + 1] = (alpha - beta).real();
        }
    }
    std::array<double, 4> x{};
    for (std::size_t j = 0; j < 4; ++j) {
        auto replaced = m;
        for (std::size_t i = 0; i < 4; ++i) {
            replaced[i][j] = rhs[i];
        }
        x[j] = determinant4(replaced) / determinant4(m);
    }
    return {{{x[0], x[1]}, {x[2], x[3]}}};
}

std::vector<PhdChannelBoundary> boundaries(const PhdFrozenSlice &f) {
    std::vector<PhdChannelBoundary> result;
    for (std::size_t i = 0; i < f.coefficients().channels.size(); ++i) {
        result.push_back({f.coefficients().channels[i], f.nominal_incident()[i], {}});
    }
    return result;
}

std::vector<PhdPerturbationLimit> limits(const PhdFrozenSlice &f, double bound = 2.) {
    std::vector<PhdPerturbationLimit> result;
    for (std::size_t i = 0; i < f.coefficients().channels.size(); ++i) {
        if (i != f.coefficients().pump_channel) {
            result.push_back({f.coefficients().channels[i], bound});
        }
    }
    return result;
}

const PhdNetworkChannelState &
state(const PhdFixedPumpResult &r, std::size_t device, std::size_t port, int harmonic) {
    for (const auto &s : r.channels) {
        if (s.device == device && s.channel.port == port && s.channel.bin == harmonic) {
            return s;
        }
    }
    throw std::runtime_error("missing result channel");
}

void invariant(const PhdFixedPumpResult &r) {
    require(r.pump_clamp_exact && r.solved_pump == r.prescribed_pump, "exact prescribed pump");
    require(std::isfinite(r.relative_residual) && r.relative_residual < 1e-12,
            "small finite residual");
}

void loaded_scalar_and_phase() {
    const auto d = fixture();
    const Complex source{.03, -.04}, gamma{.31, .17};
    const Complex expected = scalar_oracle(1. - gamma * d.direct(1, 1),
                                           -gamma * d.conjugate(1, 1),
                                           source + gamma * d.baseline_outgoing[1]);
    for (double phase : {0., .37, -.91, 1.5707963267948966}) {
        const auto f = PhdCoefficientSlice(d).freeze_pump(phase);
        auto b = boundaries(f);
        const auto p = Complex{std::cos(phase), std::sin(phase)};
        b[1].source = source * power(p, 2);
        b[1].reflection = gamma;
        const auto r = PhdFixedPumpNetwork(f, b, limits(f)).solve();
        invariant(r);
        const auto &s = state(r, 0, 0, 2);
        close(s.incident, expected * power(p, 2), "complex load real 2x2 oracle and phase");
        const auto out = (d.baseline_outgoing[1] + d.direct(1, 1) * expected +
                          d.conjugate(1, 1) * std::conj(expected)) *
                         power(p, 2);
        close(s.outgoing, out, "affine nonzero F retained once");
        close(s.incident - b[1].source - gamma * s.outgoing, 0., "loaded boundary residual");
        close(r.phd_perturbations[0], 0., "zero pump perturbation");
    }
}

void coupled_harmonics_and_permutation() {
    auto d = fixture();
    d.direct(1, 2) = {.12, -.04};
    d.direct(2, 1) = {-.08, .14};
    d.conjugate(1, 2) = {.09, .02};
    d.conjugate(2, 1) = {.07, -.06};
    const std::array<Complex, 2> gamma{{{.31, .17}, {-.21, .13}}};
    const std::array<Complex, 2> source{{{.03, -.04}, {-.02, .06}}};
    const auto expected = pair_oracle(d, gamma, source);
    for (bool permuted : {false, true}) {
        auto data = d;
        if (permuted) {
            const std::array<std::size_t, 3> order{{2, 0, 1}};
            data.pump_channel = 1;
            for (std::size_t i = 0; i < 3; ++i) {
                data.channels[i] = d.channels[order[i]];
                data.baseline_outgoing[i] = d.baseline_outgoing[order[i]];
                for (std::size_t j = 0; j < 3; ++j) {
                    data.direct(i, j) = d.direct(order[i], order[j]);
                    data.conjugate(i, j) = d.conjugate(order[i], order[j]);
                }
            }
        }
        const auto f = PhdCoefficientSlice(data).freeze_pump(0.);
        auto b = boundaries(f);
        auto l = limits(f);
        for (auto &entry : b) {
            if (entry.channel.bin > 1) {
                const auto j = static_cast<std::size_t>(entry.channel.bin - 2);
                entry.source = source[j];
                entry.reflection = gamma[j];
            }
        }
        std::reverse(b.begin(), b.end());
        std::reverse(l.begin(), l.end());
        const auto r = PhdFixedPumpNetwork(f, b, l).solve();
        invariant(r);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &s = state(r, 0, 0, static_cast<int>(i + 2));
            close(s.incident, expected[i], "two feedback harmonics Cramer oracle / labels");
            Complex outgoing = d.baseline_outgoing[i + 1];
            for (std::size_t j = 0; j < 2; ++j) {
                outgoing += d.direct(i + 1, j + 1) * expected[j] +
                            d.conjugate(i + 1, j + 1) * std::conj(expected[j]);
            }
            close(s.outgoing, outgoing, "cross harmonic outgoing and T");
        }
    }
}

PhdAffineEnvironment one_channel_environment(
    double frequency, double reference, Complex gain, Complex offset, int harmonic = 2) {
    const std::vector<ConversionChannel> channels{{0, harmonic}};
    return {FrequencyConversionModel(
                frequency, channels, SMatrix{1, {gain}}, zero(1), std::vector<Complex>{reference}),
            {{{0, harmonic}, 0., 0.}},
            {{{0, harmonic}, offset}},
            {{0, "environment:plane"}}};
}

void mixed_reference_multidevice_and_ownership() {
    auto d = fixture();
    d.port_reference_ohms = {50., 75., 30.};
    d.port_reference_planes = {"pump", "junction:A", "junction:B"};
    d.channels = {{0, 1}, {1, 2}, {2, 3}};
    const auto frozen = PhdCoefficientSlice(d).freeze_pump(0.);
    auto b = boundaries(frozen);
    auto l = limits(frozen);
    auto network = PhdFixedPumpNetwork(frozen, b, l);
    auto e = one_channel_environment(1e9, 120., {.12, -.08}, {.03, .02});
    const auto e1 = network.add_environment(e);
    const auto e2 =
        network.add_environment(one_channel_environment(1e9, 90., {-.1, .07}, {-.02, .01}, 3));
    network.connect(0, 1, e1, 0);
    network.connect(0, 2, e2, 0);
    // All caller storage may now change, and network copies outlive originals.
    d.baseline_outgoing.assign(3, 1e8);
    b.clear();
    l.clear();
    e.output_offsets[0].outgoing = 1e6;
    const auto copied = network;
    auto moved = std::move(network);
    const auto r = copied.solve(), r2 = moved.solve();
    invariant(r);
    invariant(r2);
    const std::array<double, 2> z1{{75., 30.}}, z2{{120., 90.}};
    const std::array<Complex, 2> gain{{{.12, -.08}, {-.1, .07}}}, offset{{{.03, .02}, {-.02, .01}}};
    const auto original = fixture();
    for (std::size_t i = 0; i < 2; ++i) {
        // V continuity and I balance give a=A*b+B*c; c=g*d+q;
        // d=B*b-A*c. Eliminating the environment yields a=R*b+Q.
        const double A = (z2[i] - z1[i]) / (z1[i] + z2[i]);
        const double B = 2. * std::sqrt(z1[i] * z2[i]) / (z1[i] + z2[i]);
        const Complex R = A + B * B * gain[i] / (1. + A * gain[i]);
        const Complex Q = B * offset[i] / (1. + A * gain[i]);
        const auto a = scalar_oracle(1. - R * original.direct(i + 1, i + 1),
                                     -R * original.conjugate(i + 1, i + 1),
                                     Q + R * original.baseline_outgoing[i + 1]);
        const auto &s = state(r, 0, i + 1, static_cast<int>(i + 2));
        const auto &t = state(r, i + 1, 0, static_cast<int>(i + 2));
        close(s.incident, a, "mixed reference independent affine elimination");
        close(std::sqrt(z1[i]) * (s.incident + s.outgoing),
              std::sqrt(z2[i]) * (t.incident + t.outgoing),
              "junction physical V continuity");
        close((s.incident - s.outgoing) / std::sqrt(z1[i]) +
                  (t.incident - t.outgoing) / std::sqrt(z2[i]),
              0.,
              "junction physical I balance");
        close(t.outgoing, gain[i] * t.incident + offset[i], "owned environment affine source");
        close(s.incident,
              state(r2, 0, i + 1, static_cast<int>(i + 2)).incident,
              "copy move lifetime");
        close(s.reference_ohms, z1[i], "native PHD reference retained");
        require(t.reference_plane == "environment:plane", "reference plane metadata retained");
    }
}

void real_dc() {
    auto d = fixture();
    d.channels[1].bin = 0;
    d.baseline_outgoing[1] = .4;
    d.direct(1, 1) = d.conjugate(1, 1) = .15;
    const auto f = PhdCoefficientSlice(d).freeze_pump(.42);
    auto b = boundaries(f);
    b[1].source = .02;
    b[1].reflection = .25;
    const auto r = PhdFixedPumpNetwork(f, b, limits(f)).solve();
    invariant(r);
    const auto &s = state(r, 0, 0, 0);
    close(s.incident, (.02 + .25 * .4) / (1. - .25 * .3), "DC one real degree");
    require(s.incident.imag() == 0. && s.outgoing.imag() == 0., "DC exactly real");
    b[1].source = {.02, 1e-30};
    rejects<std::invalid_argument>(
        [&] {
            (void)PhdFixedPumpNetwork(f, b, limits(f));
        },
        "imaginary DC source");
    b[1].source = .02;
    b[1].reflection = {.25, 1e-30};
    rejects<std::invalid_argument>(
        [&] {
            (void)PhdFixedPumpNetwork(f, b, limits(f));
        },
        "imaginary DC load");
}

void physical_cubic_error_scaling() {
    const auto f = PhdCoefficientSlice(cubic_fixture(.7, .8)).freeze_pump(.39);
    double previous = 0.;
    for (double epsilon : {.02, .01, .005, .0025}) {
        auto b = boundaries(f);
        b[0].source = .3 * epsilon;
        b[2].source = Complex{.7, -.2} * epsilon;
        b[3].source = Complex{-.2, .4} * epsilon;
        const auto r = PhdFixedPumpNetwork(f, b, limits(f, .1)).solve();
        invariant(r);
        Waves incident;
        for (const auto &s : r.channels) {
            incident.push_back(s.incident);
        }
        const auto exact = cubic_fourier(incident, .7);
        double error = 0.;
        for (std::size_t i = 0; i < exact.size(); ++i) {
            error += std::norm(exact[i] - r.channels[i].outgoing);
        }
        error = std::sqrt(error);
        require(error > 1e-9, "physical cubic must have nonzero second order remainder");
        if (previous > 0.) {
            require(previous / error > 3.9 && previous / error < 4.1,
                    "fixed pump first order approximation has quadratic error");
        }
        previous = error;
    }
}

void connection_labels_and_failures() {
    auto d = fixture();
    d.port_reference_ohms = {50., 75.};
    d.port_reference_planes = {"pump", "output"};
    d.channels = {{0, 1}, {1, 2}, {1, 3}};
    const auto f = PhdCoefficientSlice(d).freeze_pump(0.);
    PhdFixedPumpNetwork n(f, boundaries(f), limits(f));
    PhdAffineEnvironment env{
        FrequencyConversionModel(1e9,
                                 {{0, 3}, {0, 2}},
                                 zero(2),
                                 zero(2),
                                 std::vector<Complex>{{120., 15.}, {120., 15.}}),
        {{{0, 2}, 0., 0.}, {{0, 3}, 0., 0.}},
        {{{0, 2}, {.03, .02}}, {{0, 3}, {-.02, .01}}},
        {{0, "complex-reference"}}};
    auto bad = env;
    bad.boundaries[0].source = .01;
    const auto bad_id = n.add_environment(bad);
    rejects<std::invalid_argument>(
        [&] {
            n.connect(0, 1, bad_id, 0);
        },
        "connected source cannot be silently ignored");
    const auto id = n.add_environment(env);
    n.connect(0, 1, id, 0); // Failed connection above must not consume either bin.
    rejects<std::invalid_argument>(
        [&] {
            n.connect(0, 1, id, 0);
        },
        "already-connected port");
    const auto r = n.solve();
    invariant(r);
    const Complex z{120., 15.};
    for (int h : {2, 3}) {
        const auto &a = state(r, 0, 1, h), &b = state(r, id, 0, h);
        const Complex expected = h == 2 ? Complex{.03, .02} : Complex{-.02, .01};
        close(b.outgoing, expected, "permuted output offset harmonic labels");
        // Kurokawa voltage is (conj(Z)*a+Z*b)/sqrt(Re Z).
        close(std::sqrt(75.) * (a.incident + a.outgoing),
              (std::conj(z) * b.incident + z * b.outgoing) / std::sqrt(z.real()),
              "complex-reference voltage and harmonic connection mapping");
        close((a.incident - a.outgoing) / std::sqrt(75.) +
                  (b.incident - b.outgoing) / std::sqrt(z.real()),
              0.,
              "complex-reference current continuity");
    }
}

void invalid_contracts() {
    const auto f = PhdCoefficientSlice(fixture()).freeze_pump(.37);
    const auto good = boundaries(f);
    const auto bounds = limits(f);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int which = 0; which < 8; ++which) {
        auto b = good;
        if (which == 0) {
            b.pop_back();
        }
        if (which == 1) {
            b[2].channel = b[1].channel;
        }
        if (which == 2) {
            b[2].channel.bin = 9;
        }
        if (which == 3) {
            b[0].reflection = 1e-30;
        }
        if (which == 4) {
            b[0].source += Complex{1e-12, 0.};
        }
        if (which == 5) {
            b[0].source = std::conj(b[0].source);
        }
        if (which == 6) {
            b[1].source = nan;
        }
        if (which == 7) {
            b[1].reflection = nan;
        }
        rejects<std::invalid_argument>(
            [&] {
                (void)PhdFixedPumpNetwork(f, b, bounds);
            },
            "invalid labelled boundaries / pump");
    }
    for (int which = 0; which < 6; ++which) {
        auto l = bounds;
        if (which == 0) {
            l.pop_back();
        }
        if (which == 1) {
            l[1].channel = l[0].channel;
        }
        if (which == 2) {
            l[1].channel = {0, 1};
        }
        if (which == 3) {
            l[1].maximum_magnitude_sqrt_w = -1.;
        }
        if (which == 4) {
            l[1].maximum_magnitude_sqrt_w = nan;
        }
        if (which == 5) {
            l[1].maximum_magnitude_sqrt_w = std::numeric_limits<double>::infinity();
        }
        rejects<std::invalid_argument>(
            [&] {
                (void)PhdFixedPumpNetwork(f, good, l);
            },
            "invalid domain bounds");
    }
    for (const Complex tiny : {Complex{std::numeric_limits<double>::denorm_min(), 0.},
                               Complex{-std::numeric_limits<double>::denorm_min(), 0.},
                               Complex{0., std::numeric_limits<double>::denorm_min()},
                               Complex{0., -std::numeric_limits<double>::denorm_min()}}) {
        auto altered = good;
        altered[0].reflection = tiny;
        rejects<std::invalid_argument>(
            [&] {
                (void)PhdFixedPumpNetwork(f, altered, bounds);
            },
            "subnormal pump reflection");
    }
    auto changed = good;
    changed[0].source = {std::nextafter(changed[0].source.real(), 2.), changed[0].source.imag()};
    rejects<std::invalid_argument>(
        [&] {
            (void)PhdFixedPumpNetwork(f, changed, bounds);
        },
        "one ULP pump change");
    auto b = good;
    b[1].reflection = .3;
    rejects<std::domain_error>(
        [&] {
            (void)PhdFixedPumpNetwork(f, b, limits(f, .01)).solve();
        },
        "zero-source baseline feedback exceeds solved domain");
    b = good;
    (void)PhdFixedPumpNetwork(f, b, limits(f, 0.)).solve();
    b[1].source = .011;
    rejects<std::domain_error>(
        [&] {
            (void)PhdFixedPumpNetwork(f, b, limits(f, .01)).solve();
        },
        "solved input outside local validity domain");
    b[1].source = .01;
    (void)PhdFixedPumpNetwork(f, b, limits(f, .01)).solve();
    auto singular = fixture();
    singular.direct(1, 1) = 1.;
    singular.conjugate(1, 1) = 0.;
    const auto sf = PhdCoefficientSlice(singular).freeze_pump(0.);
    b = boundaries(sf);
    b[1].reflection = 1.;
    rejects<std::domain_error>(
        [&] {
            (void)PhdFixedPumpNetwork(sf, b, limits(sf)).solve();
        },
        "singular feedback rejects");
    PhdFixedPumpNetwork n(f, good, bounds);
    rejects<std::invalid_argument>(
        [&] {
            n.add_environment(one_channel_environment(1e9 + 1., 50., 0., 0.));
        },
        "wrong environment frequency");
    const auto id = n.add_environment(one_channel_environment(1e9, 50., 0., 0.));
    rejects<std::invalid_argument>(
        [&] {
            n.connect(0, 0, id, 0);
        },
        "pump physical port cannot be wired");
    rejects<std::invalid_argument>(
        [&] {
            n.connect(id, 0, id, 0);
        },
        "self connection");
    auto bad = one_channel_environment(1e9, 50., 0., 0.);
    bad.output_offsets.clear();
    rejects<std::invalid_argument>(
        [&] {
            n.add_environment(bad);
        },
        "environment affine offsets must be explicit");
    bad = one_channel_environment(1e9, 50., 0., 0.);
    bad.reference_planes.clear();
    rejects<std::invalid_argument>(
        [&] {
            n.add_environment(bad);
        },
        "environment planes must be explicit");
}
} // namespace

int main() {
    try {
        loaded_scalar_and_phase();
        coupled_harmonics_and_permutation();
        mixed_reference_multidevice_and_ownership();
        real_dc();
        physical_cubic_error_scaling();
        connection_labels_and_failures();
        invalid_contracts();
        std::cout << "Fixed pump PHD network passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
