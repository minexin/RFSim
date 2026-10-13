#include "rfmodel/models/frequency_dependent_cable.hpp"
#include "rfmodel/noise/loaded_noise.hpp"
#include "rfmodel/linear/network.hpp"
#include "rfmodel/core/power_wave_reference.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
using namespace rfmodel;
using Wide = std::complex<long double>;
constexpr long double pi = 3.141592653589793238462643383279502884L;
constexpr long double c0 = 299792458.L;
constexpr double kt = 1.380649e-23 * 307.;

// These small, explicit oracle operations do not call production parameter,
// transmission-line, renormalization, feedback, or covariance helpers.
struct Matrix {
    std::array<Wide, 4> v{};

    Wide &operator()(std::size_t row, std::size_t column) {
        return v[2 * row + column];
    }

    Wide operator()(std::size_t row, std::size_t column) const {
        return v[2 * row + column];
    }
};

Matrix identity() {
    return {{{1.L, 0.L, 0.L, 1.L}}};
}

Matrix add(const Matrix &a, const Matrix &b) {
    Matrix result;
    for (std::size_t i = 0; i < 4; ++i) {
        result.v[i] = a.v[i] + b.v[i];
    }
    return result;
}

Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix result;
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) {
            for (std::size_t k = 0; k < 2; ++k) {
                result(i, j) += a(i, k) * b(k, j);
            }
        }
    }
    return result;
}

Matrix adjoint(const Matrix &a) {
    return {{{std::conj(a(0, 0)), std::conj(a(1, 0)), std::conj(a(0, 1)), std::conj(a(1, 1))}}};
}

Matrix inverse(const Matrix &a) {
    const Wide determinant = a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0);
    require(std::abs(determinant) > 1e-18L, "singular test oracle");
    return {{{a(1, 1) / determinant,
              -a(0, 1) / determinant,
              -a(1, 0) / determinant,
              a(0, 0) / determinant}}};
}

Matrix covariance(const Matrix &transfer, const Matrix &source) {
    return multiply(multiply(transfer, source), adjoint(transfer));
}

Complex narrow(Wide value) {
    return {static_cast<double>(value.real()), static_cast<double>(value.imag())};
}

void close(Complex actual, Complex expected, const char *message, double tolerance = 3e-12) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()), message);
    require(std::abs(actual - expected) <= tolerance * std::max(1., std::abs(expected)), message);
}

void matrix_close(const SMatrix &actual,
                  const Matrix &expected,
                  const char *message,
                  double scale = 1.,
                  double tolerance = 3e-12) {
    require(actual.ports == 2 && actual.values.size() == 4, "two-port dimensions");
    for (std::size_t i = 0; i < 4; ++i) {
        close(actual.values[i] / scale, narrow(expected.v[i]), message, tolerance);
    }
}

void matrix_close(const SMatrix &actual,
                  const SMatrix &expected,
                  const char *message,
                  double scale = 1.) {
    require(actual.ports == 2 && expected.ports == 2, "two-port comparison dimensions");
    for (std::size_t i = 0; i < 4; ++i) {
        close(actual.values[i] / scale, expected.values[i] / scale, message);
    }
}

long double oracle_loss(double frequency, const FrequencyDependentCableParameters &p) {
    const long double ratio = static_cast<long double>(frequency) / p.reference_frequency_hz;
    return p.length_m * (p.sqrt_loss_db_per_m * std::sqrt(ratio) + p.linear_loss_db_per_m * ratio);
}

Matrix oracle_abcd(double frequency, const FrequencyDependentCableParameters &p) {
    const long double attenuation = oracle_loss(frequency, p) * std::log(10.L) / 20.L;
    const long double phase = 2.L * pi * frequency * p.length_m *
                              std::sqrt(static_cast<long double>(p.relative_permittivity)) / c0;
    const Wide argument{attenuation, phase};
    const Wide diagonal = std::cosh(argument), off_diagonal = std::sinh(argument);
    const long double characteristic = p.characteristic_ohms;
    return {{{diagonal, characteristic * off_diagonal, off_diagonal / characteristic, diagonal}}};
}

Matrix oracle_real_s(double frequency, const FrequencyDependentCableParameters &p) {
    const Matrix abcd = oracle_abcd(frequency, p);
    const long double reference = p.reference_ohms;
    const Wide denominator =
        abcd(0, 0) + abcd(0, 1) / reference + abcd(1, 0) * reference + abcd(1, 1);
    const Wide determinant = abcd(0, 0) * abcd(1, 1) - abcd(0, 1) * abcd(1, 0);
    return {{{((abcd(0, 0) - abcd(1, 1)) + abcd(0, 1) / reference - abcd(1, 0) * reference) /
                  denominator,
              2.L * determinant / denominator,
              2.L / denominator,
              ((abcd(1, 1) - abcd(0, 0)) + abcd(0, 1) / reference - abcd(1, 0) * reference) /
                  denominator}}};
}

// Solve V1=A*V2-B*I2 and I1=C*V2-D*I2, with both currents into
// the cable. This works directly in physical V/I, including complex references.
Matrix oracle_vi_s(double frequency,
                   const FrequencyDependentCableParameters &p,
                   const std::array<Complex, 2> &references) {
    const Matrix abcd = oracle_abcd(frequency, p);
    const Wide z0 = references[0], z1 = references[1];
    const long double r0 = std::sqrt(z0.real()), r1 = std::sqrt(z1.real());
    const Matrix coefficients{{{z0 / r0,
                                -(abcd(0, 0) * z1 + abcd(0, 1)) / r1,
                                -1.L / r0,
                                -(abcd(1, 0) * z1 + abcd(1, 1)) / r1}}};
    const Matrix drive{{{-std::conj(z0) / r0,
                         (abcd(0, 0) * std::conj(z1) - abcd(0, 1)) / r1,
                         -1.L / r0,
                         (abcd(1, 0) * std::conj(z1) - abcd(1, 1)) / r1}}};
    return multiply(inverse(coefficients), drive);
}

Matrix oracle_thermal(const Matrix &scattering) {
    Matrix result = identity();
    const Matrix product = multiply(scattering, adjoint(scattering));
    for (std::size_t i = 0; i < 4; ++i) {
        result.v[i] -= product.v[i];
    }
    return result;
}

void positive_semidefinite(const SMatrix &value, const char *message, double scale = kt) {
    const double a = value(0, 0).real() / scale, d = value(1, 1).real() / scale;
    const Complex b = value(0, 1) / scale;
    close(value(0, 0).imag() / scale, 0., message);
    close(value(1, 1).imag() / scale, 0., message);
    close(value(1, 0) / scale, std::conj(b), message);
    const double lower_eigenvalue = 0.5 * (a + d - std::hypot(a - d, 2. * std::abs(b)));
    require(a >= -3e-12 && d >= -3e-12 && lower_eigenvalue >= -3e-12, message);
}

struct OracleLoadedNoise {
    Matrix incident, outgoing;
};

OracleLoadedNoise oracle_loaded_noise(const Matrix &s,
                                      const Matrix &intrinsic,
                                      const std::vector<Complex> &gamma,
                                      const std::array<double, 2> &relative_temperatures) {
    const Matrix reflection{{{Wide(gamma[0]), 0.L, 0.L, Wide(gamma[1])}}};
    Matrix emission, feedback = identity();
    const Matrix loop = multiply(s, reflection);
    for (std::size_t i = 0; i < 4; ++i) {
        feedback.v[i] -= loop.v[i];
    }
    for (std::size_t i = 0; i < 2; ++i) {
        emission(i, i) = relative_temperatures[i] * (1.L - std::norm(Wide(gamma[i])));
    }
    const Matrix h = inverse(feedback), j = multiply(h, s);
    return {add(covariance(multiply(reflection, h), intrinsic),
                covariance(add(identity(), multiply(reflection, j)), emission)),
            add(covariance(h, intrinsic), covariance(j, emission))};
}

void check_thru(const SMatrix &value, const char *message) {
    require(value.ports == 2 && value.values.size() == 4, message);
    require(value(0, 0) == Complex{} && value(1, 1) == Complex{} && value(0, 1) == Complex{1.} &&
                value(1, 0) == Complex{1.},
            message);
}

void test_loss_law_and_analytic_cases() {
    FrequencyDependentCableParameters p;
    p.length_m = 2.5;
    p.reference_frequency_hz = 2e9;
    for (const std::array<double, 2> loss :
         {std::array<double, 2>{0., 0.}, {1.7, 0.}, {0., 0.45}, {1.7, 0.45}}) {
        p.sqrt_loss_db_per_m = loss[0];
        p.linear_loss_db_per_m = loss[1];
        for (const double ratio : {0., 0.01, 0.25, 1., 4., 9.}) {
            const double frequency = ratio * p.reference_frequency_hz;
            close(cable_propagation_loss_db(frequency, p),
                  static_cast<double>(oracle_loss(frequency, p)),
                  "independent loss law");
        }
    }
    // Neither coefficient is insertion loss: mismatch adds an independent effect.
    p.length_m = 0.31;
    p.relative_permittivity = 2.25;
    p.characteristic_ohms = p.reference_ohms = 73.;
    p.sqrt_loss_db_per_m = 3.1;
    p.linear_loss_db_per_m = 0.8;
    for (double frequency : {0., 1e6, 73e6, 530e6, 2e9}) {
        const auto actual = frequency_dependent_cable_s_parameters(frequency, p);
        const long double loss = oracle_loss(frequency, p);
        const long double phase = -2.L * pi * frequency * p.length_m * 1.5L / c0;
        const Wide propagation = std::exp(Wide{-loss * std::log(10.L) / 20.L, phase});
        const Matrix expected{{{0.L, propagation, propagation, 0.L}}};
        matrix_close(actual, expected, "matched amplitude and phase");
        close(-20. * std::log10(std::abs(actual(1, 0))),
              static_cast<double>(loss),
              "matched propagation loss in dB");
    }
    p = {};
    p.characteristic_ohms = 100.;
    p.length_m = static_cast<double>(c0) / (4. * 1e9);
    const Matrix quarter{{{0.6L, Wide{0.L, -0.8L}, Wide{0.L, -0.8L}, 0.6L}}};
    matrix_close(frequency_dependent_cable_s_parameters(1e9, p),
                 quarter,
                 "100-ohm quarter-wave physical oracle");
    p.sqrt_loss_db_per_m = 19.;
    p.linear_loss_db_per_m = 5.;
    check_thru(frequency_dependent_cable_s_parameters(0., p), "DC exact thru");
    require(cable_propagation_loss_db(0., p) == 0., "DC exact zero loss");
    p.length_m = 0.;
    p.reference_frequency_hz = std::numeric_limits<double>::min();
    p.sqrt_loss_db_per_m = p.linear_loss_db_per_m = std::numeric_limits<double>::max();
    check_thru(frequency_dependent_cable_s_parameters(std::numeric_limits<double>::max(), p),
               "zero-length exact thru with extreme finite parameters");
    require(cable_propagation_loss_db(std::numeric_limits<double>::max(), p) == 0.,
            "zero-length exact zero loss");
}

void test_moderate_s_and_model() {
    FrequencyDependentCableParameters p;
    p.length_m = 0.43;
    p.relative_permittivity = 2.7;
    p.reference_frequency_hz = 1.3e9;
    for (double reference : {25., 50., 91.}) {
        p.reference_ohms = reference;
        for (double characteristic : {31., 50., 117.}) {
            p.characteristic_ohms = characteristic;
            for (const std::array<double, 2> loss :
                 {std::array<double, 2>{0., 0.}, {2.8, 0.}, {0., 0.7}, {2.8, 0.7}}) {
                p.sqrt_loss_db_per_m = loss[0];
                p.linear_loss_db_per_m = loss[1];
                FrequencyDependentCableModel model("frequency-shaped cable", p);
                const RFDeviceModel &device = model;
                const SParameterProvider &provider = model;
                require(device.name() == "frequency-shaped cable" && device.port_count() == 2,
                        "model identity and port count");
                for (std::size_t port = 0; port < 2; ++port) {
                    require(device.port(port).id == port, "model port identity");
                    close(
                        device.port(port).reference_impedance, reference, "model native reference");
                }
                rejects<std::out_of_range>([&] {
                    device.port(2);
                });
                for (double frequency : {0., 11e6, 190e6, 1.3e9, 3.9e9}) {
                    const Matrix expected = oracle_real_s(frequency, p);
                    const auto actual = provider.s_parameters(frequency);
                    matrix_close(actual, expected, "full independent ABCD S oracle");
                    matrix_close(frequency_dependent_cable_s_parameters(frequency, p),
                                 expected,
                                 "free function independent ABCD S oracle");
                    matrix_close(actual,
                                 oracle_vi_s(frequency, p, {reference, reference}),
                                 "native-reference independent physical V/I oracle");
                    close(actual(0, 1), actual(1, 0), "cable reciprocity");
                    close(actual(0, 0), actual(1, 1), "uniform cable symmetry");
                    const auto thermal = passive_thermal_noise(actual, 307.);
                    matrix_close(thermal.watts_per_hz,
                                 oracle_thermal(expected),
                                 "thermal covariance from independent ABCD",
                                 kt);
                    positive_semidefinite(thermal.watts_per_hz, "thermal PSD");
                    if (loss[0] == 0. && loss[1] == 0.) {
                        matrix_close(thermal.watts_per_hz, Matrix{}, "lossless zero noise", kt);
                    }
                }
            }
        }
    }
}

void check_loaded_signal(LinearNetwork network,
                         std::array<std::size_t, 2> ports,
                         const std::array<Complex, 2> &references,
                         double frequency,
                         const FrequencyDependentCableParameters &p) {
    const Wide source_z{39.L, 17.L}, load_z{83.L, -29.L}, source_v{1.2L, -0.37L};
    const std::array<Wide, 2> terminations{source_z, load_z};
    for (std::size_t i = 0; i < 2; ++i) {
        const Wide reference = references[i];
        const Wide denominator = terminations[i] + std::conj(reference);
        const Wide gamma = (terminations[i] - reference) / denominator;
        const Wide emission = i == 0 ? std::sqrt(reference.real()) * source_v / denominator : 0.L;
        network.terminate(ports[i], narrow(gamma), narrow(emission));
    }
    const auto solved = network.solve();
    const Matrix abcd = oracle_abcd(frequency, p);
    const Wide v2 = source_v / (abcd(0, 0) + abcd(0, 1) / load_z +
                                source_z * (abcd(1, 0) + abcd(1, 1) / load_z));
    const std::array<Wide, 2> voltage{(abcd(0, 0) + abcd(0, 1) / load_z) * v2, v2};
    const std::array<Wide, 2> current{(abcd(1, 0) + abcd(1, 1) / load_z) * v2, -v2 / load_z};
    for (std::size_t i = 0; i < 2; ++i) {
        const Complex reference = references[i];
        const double root = std::sqrt(reference.real());
        const Complex a = solved.incident[ports[i]], b = solved.outgoing[ports[i]];
        close((std::conj(reference) * a + reference * b) / root,
              narrow(voltage[i]),
              "loaded cable physical voltage");
        close((a - b) / root, narrow(current[i]), "loaded cable physical current");
    }
    require(solved.relative_residual < 1e-12, "loaded network residual");
}

void test_cascade_loaded_signal_and_noise() {
    FrequencyDependentCableParameters full;
    full.length_m = 0.86;
    full.characteristic_ohms = 88.;
    full.reference_ohms = 67.;
    full.relative_permittivity = 2.3;
    full.reference_frequency_hz = 930e6;
    full.sqrt_loss_db_per_m = 2.6;
    full.linear_loss_db_per_m = 0.41;
    auto half = full;
    half.length_m /= 2.;
    for (double frequency : {0., 17e6, 211e6, 930e6, 2.7e9}) {
        const auto section = frequency_dependent_cable_s_parameters(frequency, half);
        const auto single = frequency_dependent_cable_s_parameters(frequency, full);
        const auto section_noise = passive_thermal_noise(section, 307.);
        const auto single_noise = passive_thermal_noise(single, 307.);
        LinearNetwork cascade(full.reference_ohms);
        cascade.add(section, full.reference_ohms);
        cascade.add(section, full.reference_ohms);
        cascade.connect(1, 2);
        const auto combined = cascade.external_s({0, 3});
        const auto combined_noise =
            cascade.external_noise({0, 3}, independent_noise({section_noise, section_noise}));
        const Matrix expected = oracle_real_s(frequency, full), noise = oracle_thermal(expected);
        matrix_close(combined, expected, "two halves independent full-length S oracle");
        matrix_close(combined, single, "two halves versus one full cable");
        matrix_close(
            combined_noise.watts_per_hz, noise, "two halves independent thermal oracle", kt);
        matrix_close(combined_noise.watts_per_hz,
                     single_noise.watts_per_hz,
                     "two halves versus full cable thermal covariance",
                     kt);
        check_loaded_signal(
            cascade, {0, 3}, {full.reference_ohms, full.reference_ohms}, frequency, full);
        LinearNetwork direct(full.reference_ohms);
        direct.add(single, full.reference_ohms);
        check_loaded_signal(
            direct, {0, 1}, {full.reference_ohms, full.reference_ohms}, frequency, full);
        for (const std::vector<Complex> &gamma :
             {std::vector<Complex>{0., 0.}, {{0.27, -0.13}, {-0.36, 0.21}}}) {
            const auto boundary = thermal_boundary_noise(gamma, {149., 411.});
            const auto joined = loaded_noise(combined, combined_noise, gamma, boundary);
            const auto whole = loaded_noise(single, single_noise, gamma, boundary);
            const auto oracle =
                oracle_loaded_noise(expected, noise, gamma, {149. / 307., 411. / 307.});
            matrix_close(joined.incident.watts_per_hz,
                         oracle.incident,
                         "loaded half-cascade incident-noise oracle",
                         kt);
            matrix_close(joined.outgoing.watts_per_hz,
                         oracle.outgoing,
                         "loaded half-cascade outgoing-noise oracle",
                         kt);
            matrix_close(joined.incident.watts_per_hz,
                         whole.incident.watts_per_hz,
                         "loaded half-cascade versus full incident noise",
                         kt);
            matrix_close(joined.outgoing.watts_per_hz,
                         whole.outgoing.watts_per_hz,
                         "loaded half-cascade versus full outgoing noise",
                         kt);
            positive_semidefinite(joined.incident.watts_per_hz, "loaded incident PSD");
            positive_semidefinite(joined.outgoing.watts_per_hz, "loaded outgoing PSD");
            const auto equilibrium = loaded_noise(
                single, single_noise, gamma, thermal_boundary_noise(gamma, {307., 307.}));
            for (std::size_t port = 0; port < 2; ++port) {
                const double net_flow = static_cast<double>(
                    (oracle.incident(port, port) - oracle.outgoing(port, port)).real());
                close(joined.net_into_device_w_per_hz[port] / kt,
                      net_flow,
                      "loaded net thermal flow oracle");
                close(equilibrium.net_into_device_w_per_hz[port] / kt,
                      0.,
                      "equal-temperature thermal equilibrium");
            }
        }
    }
}

void test_complex_external_references() {
    FrequencyDependentCableParameters p;
    p.length_m = 0.27;
    p.characteristic_ohms = 97.;
    p.reference_ohms = 61.;
    p.relative_permittivity = 3.4;
    p.reference_frequency_hz = 700e6;
    p.sqrt_loss_db_per_m = 2.2;
    p.linear_loss_db_per_m = 0.65;
    for (const std::array<Complex, 2> references :
         {std::array<Complex, 2>{Complex{25., 18.}, Complex{110., -32.}},
          std::array<Complex, 2>{Complex{83., -21.}, Complex{36., 14.}}}) {
        const std::vector<Complex> external{references[0], references[1]};
        for (double frequency : {0., 51e6, 700e6, 1.9e9}) {
            const auto native = frequency_dependent_cable_s_parameters(frequency, p);
            const auto converted =
                renormalize_power_waves(native, {p.reference_ohms, p.reference_ohms}, external);
            const Matrix expected = oracle_vi_s(frequency, p, references);
            matrix_close(converted.scattering, expected, "complex-reference physical V/I S oracle");
            const auto thermal = passive_thermal_noise(native, 307.);
            const auto changed_noise =
                renormalize_noise(native, thermal, {p.reference_ohms, p.reference_ohms}, external);
            matrix_close(changed_noise.watts_per_hz,
                         oracle_thermal(expected),
                         "complex-reference thermal covariance V/I oracle",
                         kt);
            positive_semidefinite(changed_noise.watts_per_hz, "complex-reference noise PSD");
            LinearNetwork network;
            network.add_power_waves(converted.scattering, external);
            check_loaded_signal(network, {0, 1}, references, frequency, p);
        }
    }
}

void test_phase_and_group_delay() {
    FrequencyDependentCableParameters p;
    p.length_m = 0.43;
    p.relative_permittivity = 2.7;
    p.reference_frequency_hz = 930e6;
    p.sqrt_loss_db_per_m = 1.9;
    p.linear_loss_db_per_m = 0.6;
    const long double delay =
        p.length_m * std::sqrt(static_cast<long double>(p.relative_permittivity)) / c0;
    for (double characteristic : {50., 120.}) {
        p.characteristic_ohms = characteristic;
        for (double frequency : {100e6, 320e6, 930e6}) {
            constexpr double step = 1000.;
            const auto below = frequency_dependent_cable_s_parameters(frequency - step, p);
            const auto above = frequency_dependent_cable_s_parameters(frequency + step, p);
            const double measured = -std::arg(above(1, 0) * std::conj(below(1, 0))) /
                                    (4. * static_cast<double>(pi) * step);
            // Differentiate the independent ABCD denominator analytically.
            // Loss dispersion also changes mismatched transmission phase.
            const Wide argument{oracle_loss(frequency, p) * std::log(10.L) / 20.L,
                                2.L * pi * frequency * delay};
            const long double alpha_derivative =
                p.length_m * std::log(10.L) / 20.L *
                (p.sqrt_loss_db_per_m / (2.L * std::sqrt(static_cast<long double>(frequency) *
                                                         p.reference_frequency_hz)) +
                 static_cast<long double>(p.linear_loss_db_per_m) / p.reference_frequency_hz);
            const Wide derivative{alpha_derivative, 2.L * pi * delay};
            const long double q =
                static_cast<long double>(p.characteristic_ohms) / p.reference_ohms +
                static_cast<long double>(p.reference_ohms) / p.characteristic_ohms;
            const Wide denominator = 2.L * std::cosh(argument) + q * std::sinh(argument);
            const Wide denominator_derivative =
                derivative * (2.L * std::sinh(argument) + q * std::cosh(argument));
            const double expected =
                static_cast<double>((denominator_derivative / denominator).imag() / (2.L * pi));
            require(std::abs(measured / expected - 1.) < 2e-8,
                    "phase slope versus independent analytic ABCD group delay");
            if (characteristic == p.reference_ohms) {
                require(std::abs(measured / static_cast<double>(delay) - 1.) < 2e-8,
                        "matched group delay equals physical propagation delay");
            } else if (frequency == 100e6) {
                require(std::abs(measured / static_cast<double>(delay) - 1.) > 0.02,
                        "mismatched insertion group delay differs from propagation delay");
            }
        }
    }
}

void test_extremes_and_validation() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double maximum = std::numeric_limits<double>::max();
    FrequencyDependentCableParameters p;
    p.characteristic_ohms = 100.;
    p.sqrt_loss_db_per_m = 10000.;
    const Matrix opaque{{{1.L / 3.L, 0.L, 0.L, 1.L / 3.L}}};
    const auto saturated = frequency_dependent_cable_s_parameters(1e9, p);
    matrix_close(saturated, opaque, "underflowed propagation opaque cable");
    require(saturated(0, 1) == Complex{} && saturated(1, 0) == Complex{},
            "propagation underflow explicitly accepted as zero");
    matrix_close(passive_thermal_noise(saturated, 307.).watts_per_hz,
                 oracle_thermal(opaque),
                 "opaque cable thermal covariance",
                 kt);
    p.reference_frequency_hz = 1.;
    p.sqrt_loss_db_per_m = maximum;
    rejects<std::overflow_error>([&] {
        cable_propagation_loss_db(16., p);
    });
    matrix_close(frequency_dependent_cable_s_parameters(16., p),
                 opaque,
                 "overflowing final dB loss still has finite opaque S");
    p.length_m = maximum;
    rejects<std::overflow_error>([&] {
        frequency_dependent_cable_s_parameters(maximum, p);
    });
    p.sqrt_loss_db_per_m = 0.;
    rejects<std::overflow_error>([&] {
        frequency_dependent_cable_s_parameters(maximum, p);
    });

    // Positive inputs can have representable final loss despite unrepresentable
    // intermediate ratios. These expectations require no wide floating-point type.
    p = {};
    p.reference_frequency_hz = 1e300;
    p.sqrt_loss_db_per_m = 1e300;
    close(cable_propagation_loss_db(1e-300, p), 1., "underflow-safe square-root ratio");
    p.reference_frequency_hz = 1e-300;
    p.sqrt_loss_db_per_m = 1e-300;
    close(cable_propagation_loss_db(1e300, p), 1., "overflow-safe square-root ratio");
    p.sqrt_loss_db_per_m = 0.;
    p.linear_loss_db_per_m = 1e-300;
    p.length_m = 1e-300;
    close(cable_propagation_loss_db(1e300, p), 1., "scaled linear loss product");
    p.length_m = 1.;
    p.linear_loss_db_per_m = 0.;
    require(cable_propagation_loss_db(maximum, p) == 0., "zero coefficients at extreme ratio");
    const auto lossless = frequency_dependent_cable_s_parameters(maximum, p);
    close(std::norm(lossless(1, 0)), 1., "finite representable extreme phase");

    using Member = double FrequencyDependentCableParameters::*;
    const std::array<Member, 7> fields{&FrequencyDependentCableParameters::length_m,
                                       &FrequencyDependentCableParameters::characteristic_ohms,
                                       &FrequencyDependentCableParameters::relative_permittivity,
                                       &FrequencyDependentCableParameters::reference_frequency_hz,
                                       &FrequencyDependentCableParameters::sqrt_loss_db_per_m,
                                       &FrequencyDependentCableParameters::linear_loss_db_per_m,
                                       &FrequencyDependentCableParameters::reference_ohms};
    for (Member field : fields) {
        for (double bad : {-1., infinity, -infinity, nan}) {
            auto invalid = FrequencyDependentCableParameters{};
            invalid.*field = bad;
            rejects<std::invalid_argument>([&] {
                FrequencyDependentCableModel model("invalid", invalid);
            });
            rejects<std::invalid_argument>([&] {
                cable_propagation_loss_db(1e9, invalid);
            });
            rejects<std::invalid_argument>([&] {
                frequency_dependent_cable_s_parameters(1e9, invalid);
            });
        }
    }
    for (Member field : {&FrequencyDependentCableParameters::characteristic_ohms,
                         &FrequencyDependentCableParameters::relative_permittivity,
                         &FrequencyDependentCableParameters::reference_frequency_hz,
                         &FrequencyDependentCableParameters::reference_ohms}) {
        auto invalid = FrequencyDependentCableParameters{};
        invalid.*field = 0.;
        rejects<std::invalid_argument>([&] {
            FrequencyDependentCableModel model("invalid", invalid);
        });
        rejects<std::invalid_argument>([&] {
            cable_propagation_loss_db(0., invalid);
        });
        rejects<std::invalid_argument>([&] {
            frequency_dependent_cable_s_parameters(0., invalid);
        });
    }
    const FrequencyDependentCableParameters valid;
    FrequencyDependentCableModel model("valid", valid);
    rejects<std::invalid_argument>([&] {
        FrequencyDependentCableModel empty("", valid);
    });
    for (double bad_frequency : {-1., infinity, -infinity, nan}) {
        rejects<std::invalid_argument>([&] {
            model.s_parameters(bad_frequency);
        });
        rejects<std::invalid_argument>([&] {
            cable_propagation_loss_db(bad_frequency, valid);
        });
        rejects<std::invalid_argument>([&] {
            frequency_dependent_cable_s_parameters(bad_frequency, valid);
        });
    }
    for (double characteristic : {1e300, 1e-300}) {
        p = {};
        p.characteristic_ohms = characteristic;
        rejects<std::invalid_argument>([&] {
            FrequencyDependentCableModel contrast("contrast", p);
        });
        rejects<std::invalid_argument>([&] {
            frequency_dependent_cable_s_parameters(0., p);
        });
    }
    // Large common impedance scale is valid; only contrast is constrained.
    p = {};
    p.characteristic_ohms = p.reference_ohms = 1e300;
    matrix_close(frequency_dependent_cable_s_parameters(19e6, p),
                 oracle_real_s(19e6, p),
                 "large common reference scale");
    p.characteristic_ohms = p.reference_ohms = 1e-300;
    matrix_close(frequency_dependent_cable_s_parameters(19e6, p),
                 oracle_real_s(19e6, p),
                 "small common reference scale");

    // An allowed, high impedance contrast magnifies very small line phase/loss.
    // The independent ABCD oracle does not subtract nearly equal round-trip waves.
    p = {};
    p.characteristic_ohms = 1e12;
    p.reference_ohms = 1.;
    p.length_m = 4.771345159236942e-5;
    const auto electrically_short = frequency_dependent_cable_s_parameters(1., p);
    matrix_close(
        electrically_short, oracle_real_s(1., p), "high-contrast electrically short ABCD oracle");
    close(electrically_short(0, 0), {0.2, 0.4}, "high-contrast resolved tiny phase");
    p.length_m = 1.;
    p.relative_permittivity = 1e-300;
    p.reference_frequency_hz = 1.;
    p.sqrt_loss_db_per_m = 1e-17;
    const auto tiny_loss = frequency_dependent_cable_s_parameters(1., p);
    matrix_close(tiny_loss, oracle_real_s(1., p), "high-contrast tiny-loss ABCD oracle");
    close(tiny_loss(0, 0),
          narrow(oracle_real_s(1., p)(0, 0)),
          "high-contrast resolved sub-epsilon attenuation",
          1e-18);
    require(tiny_loss(0, 0).real() > 5.7e-7, "tiny attenuation must not disappear");

    // Sum before final rounding: two half-denormal terms make one subnormal.
    p = {};
    p.length_m = 0.5;
    p.sqrt_loss_db_per_m = p.linear_loss_db_per_m = std::numeric_limits<double>::denorm_min();
    require(cable_propagation_loss_db(p.reference_frequency_hz, p) ==
                std::numeric_limits<double>::denorm_min(),
            "sum of two half-denormal attenuation terms");

    // Only the final cycles must fit: the intermediate physical delay need not.
    p = {};
    p.length_m = maximum;
    p.relative_permittivity = maximum;
    const auto large_delay =
        frequency_dependent_cable_s_parameters(std::numeric_limits<double>::min(), p);
    close(std::norm(large_delay(1, 0)), 1., "finite cycles with overflowing intermediate delay");
    p.length_m = 1e-300;
    p.relative_permittivity = 1e-300;
    const auto tiny_delay = frequency_dependent_cable_s_parameters(maximum, p);
    const long double tiny_phase = (maximum * 1e-300L) * 1e-150L / c0 * (2.L * pi);
    const double expected_imaginary = static_cast<double>(-std::sin(tiny_phase));
    require(std::abs((tiny_delay(1, 0).imag() - expected_imaginary) / expected_imaginary) < 1e-12,
            "finite phase with underflowing intermediate delay");

    // Although phase itself is below denorm_min, an allowed impedance contrast
    // produces a representable reflection. Scale the independent first-order
    // ABCD result before multiplication by length, avoiding extended precision.
    p = {};
    p.length_m = std::numeric_limits<double>::denorm_min();
    p.reference_frequency_hz = 1.;
    for (double sign : {1., -1.}) {
        p.characteristic_ohms = sign > 0. ? 1e12 : 1.;
        p.reference_ohms = sign > 0. ? 1. : 1e12;
        const auto amplified = frequency_dependent_cable_s_parameters(1., p);
        const double expected =
            sign * p.length_m * (static_cast<double>(pi) * 1e12 / static_cast<double>(c0));
        require(std::abs(amplified(0, 0).imag() - expected) <=
                    2. * std::numeric_limits<double>::denorm_min(),
                "subnormal phase amplified into representable reflection");
        require(amplified(0, 0).imag() != 0., "amplified underflow phase must not disappear");
    }

    // At exact quarter turns, a floating sin(pi) residue is not a valid oracle:
    // an allowed contrast would amplify that residue into a false reflection.
    p = {};
    p.characteristic_ohms = 1e14;
    p.reference_ohms = 1.;
    p.reference_frequency_hz = 1.;
    for (double cycles : {0.25, 0.5, 0.75, 1.}) {
        p.length_m = static_cast<double>(c0) * cycles;
        const auto exact = frequency_dependent_cable_s_parameters(1., p);
        if (cycles == 0.5 || cycles == 1.) {
            const Complex transmission = cycles == 0.5 ? -1. : 1.;
            require(exact(0, 0) == Complex{} && exact(1, 1) == Complex{},
                    "exact high-contrast half-wave reflection");
            require(exact(0, 1) == transmission && exact(1, 0) == transmission,
                    "exact high-contrast half-wave transmission");
        } else {
            require(std::abs(exact(0, 0) - Complex{1.}) <=
                            4. * std::numeric_limits<double>::epsilon() &&
                        std::abs(exact(1, 1) - Complex{1.}) <=
                            4. * std::numeric_limits<double>::epsilon(),
                    "exact high-contrast quarter-wave reflection");
            require(exact(1, 0).real() == 0. && exact(0, 1).real() == 0.,
                    "exact quarter-wave transmission quadrature");
            const double imaginary = cycles == 0.25 ? -2e-14 : 2e-14;
            require(std::abs(exact(1, 0).imag() / imaginary - 1.) < 3e-15,
                    "exact quarter-wave transmission magnitude");
        }
    }
    p.length_m = static_cast<double>(c0);
    for (double frequency : {0.5 - std::ldexp(1., -40), 0.5 + std::ldexp(1., -40)}) {
        // Known half-wave plus a small, exactly representable cycle offset.
        const long double delta = 2.L * pi * (static_cast<long double>(frequency) - 0.5L);
        const Wide diagonal = -std::cos(delta), sine{0.L, -std::sin(delta)};
        const Wide denominator = 2.L * diagonal + (1e14L + 1e-14L) * sine;
        const Wide reflected = (1e14L - 1e-14L) * sine / denominator;
        const Wide transmitted = 2.L / denominator;
        matrix_close(frequency_dependent_cable_s_parameters(frequency, p),
                     Matrix{{{reflected, transmitted, transmitted, reflected}}},
                     "high-contrast nearby-half-wave independent residual-phase oracle");
    }
}
} // namespace

int main() {
    static_assert(
        std::is_base_of<rfmodel::RFDeviceModel, rfmodel::FrequencyDependentCableModel>::value,
        "cable model must implement RFDeviceModel");
    static_assert(
        std::is_base_of<rfmodel::SParameterProvider, rfmodel::FrequencyDependentCableModel>::value,
        "cable model must implement SParameterProvider");
    try {
        test_loss_law_and_analytic_cases();
        test_moderate_s_and_model();
        test_cascade_loaded_signal_and_noise();
        test_complex_external_references();
        test_phase_and_group_delay();
        test_extremes_and_validation();
    } catch (const std::exception &error) {
        std::cerr << "frequency-dependent cable: " << error.what() << '\n';
        return 1;
    }
}
