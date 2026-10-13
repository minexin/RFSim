#include "rfmodel/conversion/conversion_operating_point.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <iomanip>
#include <iostream>
#include <string>
#include <sstream>

using namespace rfmodel;

namespace {
using Vector = std::vector<Complex>;
using Pair = std::array<Complex, 2>;
const Complex imaginary{0., 1.};
const Complex root_voltage{.6, .8};
const Pair source_voltage{{{1.2, 1.6}, 0.}};
constexpr double series_resistance = 20., source_resistance = 30.;
constexpr double dc_source = .8125, dc_voltage = .5, dc_current = .00625;
constexpr std::size_t dimension = 9;
std::size_t wave_checks = 0, derivative_checks = 0, noise_checks = 0;
double maximum_wave_error = 0., maximum_derivative_error = 0., maximum_noise_error = 0.;

SMatrix zero(std::size_t n) {
    return {n, Vector(n * n)};
}

void close(Complex actual,
           Complex expected,
           const std::string &label,
           double absolute = 2e-11,
           double relative = 2e-10) {
    const double error = std::abs(actual - expected);
    if (!std::isfinite(error) || error > absolute + relative * std::abs(expected)) {
        std::ostringstream message;
        message << std::setprecision(17) << label << ": actual=" << actual
                << ", expected=" << expected << ", error=" << error;
        throw std::runtime_error(message.str());
    }
    ++wave_checks;
    maximum_wave_error = std::max(maximum_wave_error, error);
}

// These are the defining Kurokawa equations, independent of library coordinate
// transforms, connection coefficients, and linear/network solvers.
Complex a_wave(Complex voltage, Complex current, Complex z) {
    return (voltage + z * current) / (2. * std::sqrt(z.real()));
}

Complex b_wave(Complex voltage, Complex current, Complex z) {
    return (voltage - std::conj(z) * current) / (2. * std::sqrt(z.real()));
}

Complex voltage(Complex a, Complex b, Complex z) {
    return (std::conj(z) * a + z * b) / std::sqrt(z.real());
}

Complex current(Complex a, Complex b, Complex z) {
    return (a - b) / std::sqrt(z.real());
}

// Synthetic finite-channel controlled-source law. The harmonic coefficient
// belongs to this explicitly defined RMS-channel law, not a waveform truncation.
struct Law {
    double g1 = .01, cubic = .01, g2 = .02, harmonic = .004;

    Pair currents(Pair v) const {
        return {(g1 + cubic * std::norm(v[0])) * v[0], g2 * v[1] + harmonic * v[0] * v[0]};
    }
};

// Solve only the scalar, monotone local voltage-radius equation. This is not a
// network solve and does not use any RFModel nonlinear/renormalization helper.
Pair local_voltage(const Vector &a, const Pair &z, const Law &law) {
    const auto x = 2. * std::sqrt(z[0].real()) * a[0];
    double lower = 0., upper = std::norm(x);
    for (int k = 0; k < 100; ++k) {
        const double rho = (lower + upper) * .5;
        const double residual = rho * std::norm(1. + z[0] * (law.g1 + law.cubic * rho));
        if (residual < std::norm(x)) {
            lower = rho;
        } else {
            upper = rho;
        }
    }
    const auto v1 = x / (1. + z[0] * (law.g1 + law.cubic * ((lower + upper) * .5)));
    const auto v2 =
        (2. * std::sqrt(z[1].real()) * a[1] - z[1] * law.harmonic * v1 * v1) / (1. + z[1] * law.g2);
    return {v1, v2};
}

ConversionLinearization local_law(const Vector &a, const Pair &z, const Law &law = {}) {
    const auto v = local_voltage(a, z, law);
    const auto i = law.currents(v);
    const double s1 = std::sqrt(z[0].real()), s2 = std::sqrt(z[1].real());
    const double p = law.g1 + 2. * law.cubic * std::norm(v[0]);
    const auto q = law.cubic * v[0] * v[0];
    const auto d = 1. + z[0] * p, e = z[0] * q;
    const double determinant = std::norm(d) - std::norm(e);
    const auto u = 2. * s1 * std::conj(d) / determinant;
    const auto w = -2. * s1 * e / determinant;
    auto direct = zero(2), conjugate = zero(2);
    direct(0, 0) = 1. - s1 * (p * u + q * std::conj(w));
    conjugate(0, 0) = -s1 * (p * w + q * std::conj(u));
    const auto transfer = 2. * law.harmonic * v[0] / (1. + z[1] * law.g2);
    direct(1, 0) = -s2 * transfer * u;
    conjugate(1, 0) = -s2 * transfer * w;
    direct(1, 1) = 1. - 2. * z[1].real() * law.g2 / (1. + z[1] * law.g2);
    return {FrequencyConversionModel(1e6, {{0, 1}, {0, 2}}, direct, conjugate, {z[0], z[1]}),
            {b_wave(v[0], i[0], z[0]), b_wave(v[1], i[1], z[1])}};
}

// At the known physical root, transform fixed additive Norton-current drivers
// into the device's outgoing intrinsic waves by imposing da=0 locally. This
// noise block is constructed once, never changed by the nonlinear callback.
Pair intrinsic_wave(const Pair &eta, const Pair &z, const Pair &v, const Law &law) {
    const double p = law.g1 + 2. * law.cubic * std::norm(v[0]);
    const auto q = law.cubic * v[0] * v[0];
    const auto d = 1. + z[0] * p, e = z[0] * q;
    const auto forcing = -z[0] * eta[0];
    const auto dv1 =
        (std::conj(d) * forcing - e * std::conj(forcing)) / (std::norm(d) - std::norm(e));
    const auto dv2 = -z[1] * (2. * law.harmonic * v[0] * dv1 + eta[1]) / (1. + z[1] * law.g2);
    const auto di1 = p * dv1 + q * std::conj(dv1) + eta[0];
    const auto di2 = law.g2 * dv2 + 2. * law.harmonic * v[0] * dv1 + eta[1];
    return {b_wave(dv1, di1, z[0]), b_wave(dv2, di2, z[1])};
}

struct Physical {
    Vector v = Vector(dimension), i = Vector(dimension);
};

Physical
physical_point(const Pair &v, const Pair &i, double vdc = dc_voltage, double idc = dc_current) {
    Physical result;
    for (std::size_t f = 0; f < 2; ++f) {
        result.v[f] = v[f] + series_resistance * i[f];
        result.i[f] = i[f];
        result.v[2 + f] = result.v[4 + f] = v[f];
        result.i[2 + f] = -i[f];
        result.i[4 + f] = i[f];
    }
    result.v[6] = vdc + series_resistance * idc;
    result.i[6] = idc;
    result.v[7] = result.v[8] = vdc;
    result.i[7] = -idc;
    result.i[8] = idc;
    return result;
}

std::pair<Vector, Vector> waves(const Physical &point, const Vector &references) {
    Vector a(dimension), b(dimension);
    for (std::size_t i = 0; i < dimension; ++i) {
        a[i] = a_wave(point.v[i], point.i[i], references[i]);
        b[i] = b_wave(point.v[i], point.i[i], references[i]);
    }
    return {a, b};
}

ConversionNoise covariance(const std::vector<Vector> &columns, std::size_t n) {
    ConversionNoise noise{zero(n), zero(n)};
    for (const auto &column : columns) {
        require(column.size() == n, "noise driver dimension differs");
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                noise.covariance(i, j) += column[i] * std::conj(column[j]);
                noise.complementary(i, j) += column[i] * column[j];
            }
        }
    }
    return noise;
}

struct Driver {
    Pair source{}, norton{};
    double dc = 0.;
    Pair series{};
    double dc_series = 0., dc_norton = 0.;
};

// Distinct real unit-variance random drivers create deliberately improper,
// cross-frequency noise. Keep physical values independent of wave references.
const std::vector<Driver> source_drivers{{{Complex{1.1e-5, .3e-5}, Complex{-.4e-5, .7e-5}}, {}, 0.},
                                         {{Complex{-.2e-5, .8e-5}, Complex{.5e-5, .1e-5}}, {}, 0.}};
const std::vector<Driver> intrinsic_drivers{{{}, {{{1.3e-7, -.4e-7}, {.2e-7, .7e-7}}}, 0.},
                                            {{}, {{{-.5e-7, .6e-7}, {.8e-7, -.3e-7}}}, 0.}};
const std::vector<Driver> additional_drivers{
    {{Complex{.7e-5, -.6e-5}, Complex{.2e-5, .4e-5}}, {}, 1.2e-5},
    {{Complex{-.3e-5, .2e-5}, Complex{.5e-5, -.1e-5}}, {}, -.8e-5}};
const std::vector<Driver> dc_drivers{{{}, {}, .9e-5}};
const std::vector<Driver> resistor_drivers{{{}, {}, 0., {{{.6e-5, -.2e-5}, {-.3e-5, .5e-5}}}},
                                           {{}, {}, 0., {{{.1e-5, .4e-5}, {.2e-5, -.1e-5}}}}};
const std::vector<Driver> dc_resistor_drivers{{{}, {}, 0., {}, .7e-5}};
const std::vector<Driver> dc_intrinsic_drivers{{{}, {}, 0., {}, 0., 1.1e-7}};

// Independent loaded-noise oracle. Rotating onto the radial/tangential axes
// of V1=.6+.8j gives the physical denominators 1+50*.04=3 and
// 1+50*.02=2. The harmonic-current perturbation then drives frequency 2.
Physical noise_response(const Driver &driver) {
    const Pair forcing{driver.source[0] - driver.series[0], driver.source[1] - driver.series[1]};
    const auto rotated = std::conj(root_voltage) * (forcing[0] - 50. * driver.norton[0]);
    const auto dv1 = root_voltage * Complex{rotated.real() / 3., rotated.imag() / 2.};
    const auto dv2 = (forcing[1] - 50. * (.008 * root_voltage * dv1 + driver.norton[1])) / 2.;
    const Pair v{dv1, dv2};
    const Pair i{(forcing[0] - dv1) / 50., (forcing[1] - dv2) / 50.};
    const double edc = driver.dc - driver.dc_series;
    const double vdc = (8. / 15.) * (edc - 50. * driver.dc_norton);
    auto result = physical_point(v, i, vdc, (edc - vdc) / 50.);
    for (std::size_t f = 0; f < 2; ++f) {
        result.v[f] += driver.series[f];
    }
    result.v[6] += driver.dc_series;
    return result;
}

ConversionLinearization dc_law(const Vector &a, double z, Law law = {}) {
    const Pair references{z, z};
    const auto v = local_voltage({a[0], 0.}, references, law)[0].real();
    const double i = (law.g1 + law.cubic * v * v) * v;
    const double derivative = law.g1 + 3. * law.cubic * v * v;
    const double scattering = (1. - z * derivative) / (1. + z * derivative);
    return {FrequencyConversionModel(1e6, {{0, 0}}, {1, {scattering}}, zero(1), {Complex{z}}),
            {b_wave(v, i, z)}};
}

struct Fixture {
    Vector references;
    std::vector<ConversionDevice> devices;
    std::vector<ConversionPortConnection> connections{{0, 1, 1, 0}, {2, 1, 3, 0}};
    std::vector<ConversionNonlinearDevice> nonlinear;
    ConversionNoise additional{zero(dimension), zero(dimension)};
};

Fixture fixture(Vector z, Law law = {}) {
    Fixture result;
    result.references = std::move(z);
    auto resistor = zero(4);
    for (std::size_t f = 0; f < 2; ++f) {
        const auto zl = result.references[f], zr = result.references[2 + f];
        const auto denominator = series_resistance + zl + zr;
        resistor(f, f) = 1. - 2. * zl.real() / denominator;
        resistor(2 + f, 2 + f) = 1. - 2. * zr.real() / denominator;
        resistor(f, 2 + f) = resistor(2 + f, f) =
            2. * std::sqrt(zl.real()) * std::sqrt(zr.real()) / denominator;
    }
    const Pair local_z{result.references[4], result.references[5]};
    const auto nonlinear_model = local_law(Vector(2), local_z, law).jacobian;
    const auto zd = result.references[6];
    const auto zr = result.references[7];
    const auto zn = result.references[8];
    const auto ddc = 20. + zd + zr;
    SMatrix dc_resistor{2,
                        {1. - 2. * zd / ddc,
                         2. * std::sqrt(zd.real() * zr.real()) / ddc,
                         2. * std::sqrt(zd.real() * zr.real()) / ddc,
                         1. - 2. * zr / ddc}};
    std::vector<FrequencyConversionModel> models{
        FrequencyConversionModel(1e6,
                                 {{0, 1}, {0, 2}, {1, 1}, {1, 2}},
                                 resistor,
                                 zero(4),
                                 {result.references[0],
                                  result.references[1],
                                  result.references[2],
                                  result.references[3]}),
        nonlinear_model,
        FrequencyConversionModel(1e6, {{0, 0}, {1, 0}}, dc_resistor, zero(2), {zd, zr}),
        dc_law({0.}, zn.real(), law).jacobian};
    for (auto &model : models) {
        const auto n = model.channels().size();
        result.devices.push_back(
            {model, Vector(n), Vector(n), model.zero_noise(), model.zero_noise()});
    }
    for (std::size_t f = 0; f < 2; ++f) {
        const auto zf = result.references[f];
        result.devices[0].source[f] =
            std::sqrt(zf.real()) * source_voltage[f] / (source_resistance + std::conj(zf));
        result.devices[0].reflection[f] =
            (source_resistance - zf) / (source_resistance + std::conj(zf));
    }
    result.devices[2].source[0] = std::sqrt(zd.real()) * dc_source / (source_resistance + zd);
    result.devices[2].reflection[0] = (source_resistance - zd) / (source_resistance + zd);
    result.nonlinear.push_back({1, [local_z, law](const Vector &a) {
                                    return local_law(a, local_z, law);
                                }});
    result.nonlinear.push_back({3, [zn, law](const Vector &a) {
                                    return dc_law(a, zn.real(), law);
                                }});

    const auto boundary_column = [&](const Driver &driver) {
        Vector column(dimension);
        for (std::size_t f = 0; f < 2; ++f) {
            const auto zf = result.references[f];
            column[f] =
                std::sqrt(zf.real()) * driver.source[f] / (source_resistance + std::conj(zf));
        }
        column[6] = std::sqrt(zd.real()) * driver.dc / (source_resistance + zd);
        return column;
    };
    std::vector<Vector> columns;
    for (const auto &driver : source_drivers) {
        const auto column = boundary_column(driver);
        columns.emplace_back(column.begin(), column.begin() + 4);
    }
    result.devices[0].source_noise = covariance(columns, 4);
    columns.clear();
    for (const auto &driver : additional_drivers) {
        columns.push_back(boundary_column(driver));
    }
    result.additional = covariance(columns, dimension);
    columns.clear();
    for (const auto &driver : dc_drivers) {
        columns.push_back({boundary_column(driver)[6], 0.});
    }
    result.devices[2].source_noise = covariance(columns, 2);
    columns.clear();
    const Pair physical_root{root_voltage, {.028, -.096}};
    for (const auto &driver : intrinsic_drivers) {
        const auto column = intrinsic_wave(driver.norton, local_z, physical_root, law);
        columns.push_back({column[0], column[1]});
    }
    result.devices[1].intrinsic_noise = covariance(columns, 2);
    columns.clear();
    for (const auto &driver : resistor_drivers) {
        Vector column(4);
        for (std::size_t f = 0; f < 2; ++f) {
            const auto zl = result.references[f], zright = result.references[2 + f];
            const auto di = -driver.series[f] / (20. + zl + zright);
            column[f] = -std::sqrt(zl.real()) * di;
            column[2 + f] = std::sqrt(zright.real()) * di;
        }
        columns.push_back(column);
    }
    result.devices[0].intrinsic_noise = covariance(columns, 4);
    columns.clear();
    for (const auto &driver : dc_resistor_drivers) {
        const auto di = -driver.dc_series / ddc;
        columns.push_back({-std::sqrt(zd.real()) * di, std::sqrt(zr.real()) * di});
    }
    result.devices[2].intrinsic_noise = covariance(columns, 2);
    columns.clear();
    for (const auto &driver : dc_intrinsic_drivers) {
        const double derivative = law.g1 + 3. * law.cubic * dc_voltage * dc_voltage;
        columns.push_back({-std::sqrt(zn.real()) * driver.dc_norton / (1. + zn * derivative)});
    }
    result.devices[3].intrinsic_noise = covariance(columns, 1);
    return result;
}

const std::vector<Vector> reference_sets{
    Vector(dimension, 50.),
    {45., 81., 19., 32., 73., 25., 25., 80., 43.},
    {{45., 7.}, {81., -16.}, {19., -11.}, {32., 9.}, {73., 21.}, {25., -13.}, 25., 80., 43.},
    {{12., -18.}, {125., 31.}, {91., 24.}, {15., -17.}, {27., -8.}, {140., 52.}, 100., 17., 66.}};

void finite_differences(const Fixture &f) {
    const Pair z{f.references[4], f.references[5]};
    const Law law;
    for (const Pair &v : std::vector<Pair>{{0., 0.},
                                           {root_voltage, {.028, -.096}},
                                           {Complex{-.27, .41}, Complex{.15, -.22}},
                                           {Complex{1.2, -.7}, Complex{-.4, .3}}}) {
        const auto i = law.currents(v);
        const Vector a{a_wave(v[0], i[0], z[0]), a_wave(v[1], i[1], z[1])};
        const auto base = local_law(a, z);
        for (std::size_t row = 0; row < 2; ++row) {
            close(base.outgoing[row], b_wave(v[row], i[row], z[row]), "callback physical output");
        }
        constexpr double step = 1e-6;
        for (std::size_t column = 0; column < 2; ++column) {
            for (const auto direction : {Complex{1.}, imaginary}) {
                auto plus = a, minus = a;
                plus[column] += step * direction;
                minus[column] -= step * direction;
                const auto high = local_law(plus, z).outgoing;
                const auto low = local_law(minus, z).outgoing;
                for (std::size_t row = 0; row < 2; ++row) {
                    const auto analytic =
                        base.jacobian.direct()(row, column) * direction +
                        base.jacobian.conjugate()(row, column) * std::conj(direction);
                    const double error = std::abs((high[row] - low[row]) / (2. * step) - analytic);
                    require(error < 3e-8, "analytic RF Jacobian differs from central differences");
                    ++derivative_checks;
                    maximum_derivative_error = std::max(maximum_derivative_error, error);
                }
            }
        }
    }
    const double zd = f.references[8].real();
    for (double v : {0., .5, -.37, 1.1}) {
        const double i = (.01 + .01 * v * v) * v;
        const auto a = a_wave(v, i, zd);
        const auto base = dc_law({a}, zd);
        const auto high = dc_law({a + 1e-6}, zd).outgoing[0];
        const auto low = dc_law({a - 1e-6}, zd).outgoing[0];
        const auto derivative = base.jacobian.direct()(0, 0) + base.jacobian.conjugate()(0, 0);
        const double error = std::abs((high - low) / 2e-6 - derivative);
        require(error < 3e-8, "analytic DC Jacobian differs from central differences");
        close(base.outgoing[0], b_wave(v, i, zd), "DC callback physical output");
        require(base.outgoing[0].imag() == 0., "DC callback acquired imaginary part");
        ++derivative_checks;
        maximum_derivative_error = std::max(maximum_derivative_error, error);
    }
}

void check_accepted_laws(const Fixture &f,
                         const ConversionOperatingResult &result,
                         const Vector &fixed = {}) {
    const std::array<std::size_t, 4> offsets{0, 4, 6, 8};
    for (std::size_t d = 0; d < f.devices.size(); ++d) {
        const auto first = offsets[d], n = f.devices[d].model.channels().size();
        const Vector local(result.waves.incident.begin() + first,
                           result.waves.incident.begin() + first + n);
        auto expected = d == 1   ? local_law(local, {f.references[4], f.references[5]})
                        : d == 3 ? dc_law(local, f.references[8].real())
                                 : ConversionLinearization{f.devices[d].model, Vector(n)};
        if (d == 0 || d == 2) {
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    expected.outgoing[i] += expected.jacobian.direct()(i, j) * local[j];
                }
            }
        }
        const auto &actual = result.linearized_devices[d].model;
        require(actual.reference_impedances_ohms() ==
                    f.devices[d].model.reference_impedances_ohms(),
                "accepted Jacobian references changed");
        require(actual.spacing_hz() == 1e6, "accepted Jacobian grid changed");
        for (std::size_t i = 0; i < n; ++i) {
            require(actual.channels()[i].port == expected.jacobian.channels()[i].port &&
                        actual.channels()[i].bin == expected.jacobian.channels()[i].bin,
                    "accepted Jacobian channel order changed");
            Complex offset = expected.outgoing[i] + (fixed.empty() ? Complex{} : fixed[first + i]);
            for (std::size_t j = 0; j < n; ++j) {
                close(actual.direct()(i, j),
                      expected.jacobian.direct()(i, j),
                      "accepted direct derivative",
                      3e-14,
                      3e-13);
                close(actual.conjugate()(i, j),
                      expected.jacobian.conjugate()(i, j),
                      "accepted conjugate derivative",
                      3e-14,
                      3e-13);
                offset -= expected.jacobian.direct()(i, j) * local[j] +
                          expected.jacobian.conjugate()(i, j) * std::conj(local[j]);
            }
            close(result.output_offset[first + i], offset, "accepted affine offset", 3e-14, 3e-13);
            close(result.waves.outgoing[first + i],
                  expected.outgoing[i] + (fixed.empty() ? Complex{} : fixed[first + i]),
                  "true accepted F(a)",
                  3e-14,
                  3e-13);
        }
    }
}

void check_physical(const Fixture &f,
                    const ConversionOperatingResult &result,
                    const Physical &expected) {
    const auto wanted = waves(expected, f.references);
    for (std::size_t i = 0; i < dimension; ++i) {
        const auto a = result.waves.incident[i], b = result.waves.outgoing[i], z = f.references[i];
        close(a, wanted.first[i], "physical incident wave");
        close(b, wanted.second[i], "physical outgoing wave");
        close(voltage(a, b, z), expected.v[i], "physical voltage", 3e-10);
        close(current(a, b, z), expected.i[i], "physical current");
        if (i >= 6) {
            require(a.imag() == 0. && b.imag() == 0., "DC waves are not real");
        }
    }
    for (const auto &wire : {std::pair<std::size_t, std::size_t>{2, 4}, {3, 5}, {7, 8}}) {
        const auto vi = voltage(result.waves.incident[wire.first],
                                result.waves.outgoing[wire.first],
                                f.references[wire.first]);
        const auto vj = voltage(result.waves.incident[wire.second],
                                result.waves.outgoing[wire.second],
                                f.references[wire.second]);
        const auto ii = current(result.waves.incident[wire.first],
                                result.waves.outgoing[wire.first],
                                f.references[wire.first]);
        const auto ij = current(result.waves.incident[wire.second],
                                result.waves.outgoing[wire.second],
                                f.references[wire.second]);
        close(vi, vj, "wire voltage continuity", 3e-10);
        close(ii, -ij, "wire inward current conservation");
    }
    require(result.scaled_residual <= 1., "accepted residual exceeds tolerance");
}

void noise_close(Complex actual, Complex expected, const char *label) {
    const double error = std::abs(actual - expected);
    if (!std::isfinite(error) || error > 3e-24 + 3e-9 * std::abs(expected)) {
        std::ostringstream message;
        message << std::setprecision(17) << label << ": actual=" << actual
                << ", expected=" << expected;
        throw std::runtime_error(message.str());
    }
    ++noise_checks;
    maximum_noise_error = std::max(maximum_noise_error, error);
}

void check_noise(const Fixture &f, const ConversionResult &result) {
    std::vector<Vector> a_columns, b_columns, voltage_columns, current_columns;
    for (const auto *group : {&source_drivers,
                              &intrinsic_drivers,
                              &additional_drivers,
                              &dc_drivers,
                              &resistor_drivers,
                              &dc_resistor_drivers,
                              &dc_intrinsic_drivers}) {
        for (const auto &driver : *group) {
            const auto physical = noise_response(driver);
            const auto column = waves(physical, f.references);
            a_columns.push_back(column.first);
            b_columns.push_back(column.second);
            voltage_columns.push_back(physical.v);
            current_columns.push_back(physical.i);
        }
    }
    const auto aa = covariance(a_columns, dimension), bb = covariance(b_columns, dimension);
    const auto vv = covariance(voltage_columns, dimension),
               ii = covariance(current_columns, dimension);
    ConversionNoise ab{zero(dimension), zero(dimension)};
    for (std::size_t k = 0; k < a_columns.size(); ++k) {
        for (std::size_t i = 0; i < dimension; ++i) {
            for (std::size_t j = 0; j < dimension; ++j) {
                ab.covariance(i, j) += a_columns[k][i] * std::conj(b_columns[k][j]);
                ab.complementary(i, j) += a_columns[k][i] * b_columns[k][j];
            }
        }
    }
    require(std::abs(ab.covariance(0, 8)) > 1e-15 && std::abs(bb.complementary(4, 5)) > 1e-15,
            "fixture lacks cross-device or improper cross-frequency noise");
    for (std::size_t i = 0; i < dimension; ++i) {
        for (std::size_t j = 0; j < dimension; ++j) {
            noise_close(result.incident_noise.covariance(i, j), aa.covariance(i, j), "incident C");
            noise_close(
                result.incident_noise.complementary(i, j), aa.complementary(i, j), "incident P");
            noise_close(result.outgoing_noise.covariance(i, j), bb.covariance(i, j), "outgoing C");
            noise_close(
                result.outgoing_noise.complementary(i, j), bb.complementary(i, j), "outgoing P");
            noise_close(result.incident_outgoing_noise.covariance(i, j),
                        ab.covariance(i, j),
                        "incident-first cross C");
            noise_close(result.incident_outgoing_noise.complementary(i, j),
                        ab.complementary(i, j),
                        "incident-first cross P");
            // Reconstruct physical noise from the reported a/b/cross matrices.
            // This also verifies reference invariance without comparing raw waves.
            for (bool is_current : {false, true}) {
                const auto zi = f.references[i], zj = f.references[j];
                const auto x = (is_current ? Complex{1.} : std::conj(zi)) / std::sqrt(zi.real());
                const auto y = (is_current ? Complex{-1.} : zi) / std::sqrt(zi.real());
                const auto u = (is_current ? Complex{1.} : std::conj(zj)) / std::sqrt(zj.real());
                const auto v = (is_current ? Complex{-1.} : zj) / std::sqrt(zj.real());
                const auto c =
                    x * std::conj(u) * result.incident_noise.covariance(i, j) +
                    x * std::conj(v) * result.incident_outgoing_noise.covariance(i, j) +
                    y * std::conj(u) * std::conj(result.incident_outgoing_noise.covariance(j, i)) +
                    y * std::conj(v) * result.outgoing_noise.covariance(i, j);
                const auto p = x * u * result.incident_noise.complementary(i, j) +
                               x * v * result.incident_outgoing_noise.complementary(i, j) +
                               y * u * result.incident_outgoing_noise.complementary(j, i) +
                               y * v * result.outgoing_noise.complementary(i, j);
                noise_close(c, (is_current ? ii : vv).covariance(i, j), "physical V/I covariance");
                noise_close(
                    p, (is_current ? ii : vv).complementary(i, j), "physical V/I complementary");
            }
        }
        noise_close(result.net_noise_into_device_w_per_hz[i],
                    aa.covariance(i, i) - bb.covariance(i, i),
                    "net noise");
        if (i >= 6) {
            noise_close(result.incident_noise.covariance(i, i),
                        result.incident_noise.complementary(i, i),
                        "DC incident C=P");
            noise_close(result.outgoing_noise.covariance(i, i),
                        result.outgoing_noise.complementary(i, i),
                        "DC outgoing C=P");
            noise_close(result.incident_outgoing_noise.covariance(i, i),
                        result.incident_outgoing_noise.complementary(i, i),
                        "DC cross C=P");
        }
    }
    for (const auto &wire : {std::pair<std::size_t, std::size_t>{2, 4}, {3, 5}, {7, 8}}) {
        noise_close(result.net_noise_into_device_w_per_hz[wire.first] +
                        result.net_noise_into_device_w_per_hz[wire.second],
                    0.,
                    "wire net-noise conservation");
    }
}

void physical_operating_points() {
    const Law law;
    const Pair root{root_voltage, {.028, -.096}};
    const auto expected = physical_point(root, law.currents(root));
    ConversionOperatingOptions options;
    options.relative_tolerance = 1e-12;
    options.absolute_tolerance = 1e-14;
    for (std::size_t configuration = 0; configuration < reference_sets.size(); ++configuration) {
        const auto f = fixture(reference_sets[configuration]);
        finite_differences(f);
        const auto exact = waves(expected, f.references).first;
        const auto warm = solve_conversion_operating_point_power_waves(
            f.devices, f.connections, f.nonlinear, exact, options, true, &f.additional);
        require(warm.iterations == 0,
                "exact physical warm start did not converge in zero iterations");
        require(std::abs(warm.linearized_devices[1].model.conjugate()(0, 0)) > .01 &&
                    std::abs(warm.linearized_devices[1].model.conjugate()(1, 0)) > .001 &&
                    std::abs(warm.linearized_devices[1].model.direct()(1, 0)) > .01,
                "fixture lost conjugate or cross-frequency Jacobian response");
        check_physical(f, warm, expected);
        check_accepted_laws(f, warm);
        check_noise(f, warm.waves);
        const auto ordinary = solve_conversion_operating_point_power_waves(
            f.devices, f.connections, f.nonlinear, {}, options, true, &f.additional);
        require(ordinary.iterations > 0, "default start unexpectedly exact");
        check_physical(f, ordinary, expected);
        check_accepted_laws(f, ordinary);
        check_noise(f, ordinary.waves);
        auto displaced = exact;
        for (std::size_t i = 0; i < dimension; ++i) {
            displaced[i] = .31 * exact[i] + (i >= 6 ? Complex{-.013} : Complex{.017, -.009});
        }
        const auto moved = solve_conversion_operating_point_power_waves(
            f.devices, f.connections, f.nonlinear, displaced, options, true, &f.additional);
        check_physical(f, moved, expected);
        check_accepted_laws(f, moved);
        check_noise(f, moved.waves);
        // A deliberately loose, already-acceptable point must be returned with
        // its true F(a), not silently replaced by one additional affine step.
        auto acceptable = exact;
        acceptable[4] += Complex{2e-5, -1e-5};
        ConversionOperatingOptions loose;
        loose.relative_tolerance = .009;
        loose.absolute_tolerance = 1e-4;
        const auto retained = solve_conversion_operating_point_power_waves(
            f.devices, f.connections, f.nonlinear, acceptable, loose, true, &f.additional);
        require(retained.iterations == 0 && retained.waves.incident == acceptable,
                "accepted nonlinear incident point was replaced");
        check_accepted_laws(f, retained);
        // Separate snapshot-consistency check, not the physical noise oracle:
        // replay only the returned affine artifacts, without any callback.
        const auto replay =
            solve_conversion_operating_point_power_waves(retained.linearized_devices,
                                                         f.connections,
                                                         {},
                                                         {},
                                                         options,
                                                         true,
                                                         &f.additional,
                                                         retained.output_offset);
        const std::array<const ConversionNoise *, 3> original_noise{
            &retained.waves.incident_noise,
            &retained.waves.outgoing_noise,
            &retained.waves.incident_outgoing_noise};
        const std::array<const ConversionNoise *, 3> replay_noise{
            &replay.waves.incident_noise,
            &replay.waves.outgoing_noise,
            &replay.waves.incident_outgoing_noise};
        for (std::size_t pair = 0; pair < original_noise.size(); ++pair) {
            for (std::size_t i = 0; i < dimension; ++i) {
                for (std::size_t j = 0; j < dimension; ++j) {
                    noise_close(replay_noise[pair]->covariance(i, j),
                                original_noise[pair]->covariance(i, j),
                                "snapshot replay C");
                    noise_close(replay_noise[pair]->complementary(i, j),
                                original_noise[pair]->complementary(i, j),
                                "snapshot replay P");
                }
            }
        }
        for (std::size_t i = 0; i < dimension; ++i) {
            noise_close(replay.waves.net_noise_into_device_w_per_hz[i],
                        retained.waves.net_noise_into_device_w_per_hz[i],
                        "snapshot replay net noise");
        }
        if (configuration == 0) {
            const auto legacy = solve_conversion_operating_point(
                f.devices, f.connections, f.nonlinear, displaced, options, true, &f.additional);
            require(legacy.iterations == moved.iterations && legacy.backtracks == moved.backtracks,
                    "common-real iteration diagnostics differ from legacy");
            for (std::size_t i = 0; i < dimension; ++i) {
                close(legacy.waves.incident[i],
                      moved.waves.incident[i],
                      "common-real incident parity");
                close(legacy.waves.outgoing[i],
                      moved.waves.outgoing[i],
                      "common-real outgoing parity");
                close(legacy.output_offset[i], moved.output_offset[i], "common-real affine parity");
            }
            check_noise(f, legacy.waves);
        }
    }
}

void linear_and_affine_limits() {
    Law law;
    law.cubic = law.harmonic = 0.;
    for (const auto &references : reference_sets) {
        auto f = fixture(references, law);
        // The physical current offsets map to fixed outgoing waves on both
        // connected nonlinear RF channels and the connected DC channel.
        const Pair eta{Complex{.001, .002}, Complex{-.0007, .0003}};
        constexpr double eta_dc = .0011;
        for (bool affine : {false, true}) {
            const auto j = affine ? eta : Pair{};
            const double jd = affine ? eta_dc : 0.;
            const Pair v{(source_voltage[0] - 50. * j[0]) / (1. + 50. * law.g1),
                         (source_voltage[1] - 50. * j[1]) / (1. + 50. * law.g2)};
            const Pair i{law.g1 * v[0] + j[0], law.g2 * v[1] + j[1]};
            const double vd = (dc_source - 50. * jd) / (1. + 50. * law.g1);
            const auto expected = physical_point(v, i, vd, law.g1 * vd + jd);
            Vector fixed(dimension);
            const auto rf_offset = intrinsic_wave(j, {references[4], references[5]}, v, law);
            fixed[4] = rf_offset[0];
            fixed[5] = rf_offset[1];
            fixed[8] = -std::sqrt(references[8].real()) * jd / (1. + references[8] * law.g1);
            const auto result = solve_conversion_operating_point_power_waves(
                f.devices, f.connections, {}, {}, {}, false, nullptr, fixed);
            check_physical(f, result, expected);
            require(result.iterations == 1,
                    "affine empty-callback limit did not solve in one step");
            for (std::size_t k = 0; k < dimension; ++k) {
                close(result.output_offset[k], fixed[k], "empty-callback fixed affine offset");
            }
            if (!affine) {
                const auto callbacks = solve_conversion_operating_point_power_waves(
                    f.devices, f.connections, f.nonlinear);
                check_physical(f, callbacks, expected);
            }
        }
    }
}
} // namespace

int main() {
    try {
        physical_operating_points();
        linear_and_affine_limits();
        std::cout << std::setprecision(17) << "{\"passed\":true,\"wave_checks\":" << wave_checks
                  << ",\"derivative_checks\":" << derivative_checks
                  << ",\"noise_checks\":" << noise_checks
                  << ",\"maximum_wave_absolute_error\":" << maximum_wave_error
                  << ",\"maximum_derivative_absolute_error\":" << maximum_derivative_error
                  << ",\"maximum_noise_absolute_error\":" << maximum_noise_error << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
