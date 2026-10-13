#include "rfmodel/conversion/conversion_power_wave.hpp"
#include "rfmodel/conversion/conversion_network.hpp"
#include "rfmodel/conversion/conversion_operating_point.hpp"
#include "rfmodel/models/rlc_model.hpp"
#include "../support/test_support.hpp"
#include <iomanip>
#include <iostream>
#include <limits>

using namespace rfmodel;
using Vector = std::vector<Complex>;
static double maximum_wave_error = 0., maximum_noise_error = 0.;
static std::size_t wave_checks = 0, noise_checks = 0;
static double maximum_wide_reference_relative_error = 0.;
static std::size_t wide_reference_checks = 0;
static std::size_t boundary_fixture_checks = 0;
static double maximum_boundary_fixture_scaled_error = 0.;

static void relative_value(Complex actual, Complex expected) {
    const double error = std::abs(actual - expected) / std::abs(expected);
    require(std::isfinite(error) && error < 1e-12, "wide-reference relative precision lost");
    maximum_wide_reference_relative_error = std::max(maximum_wide_reference_relative_error, error);
    ++wide_reference_checks;
}

static void close_value(Complex actual, Complex expected, bool noise = false) {
    const auto error = std::abs(actual - expected);
    const auto tolerance = (noise ? 1e-29 : 1e-12) + 2e-10 * std::abs(expected);
    require(std::isfinite(error) && error <= tolerance,
            noise ? "physical noise mismatch" : "physical wave mismatch");
    if (noise) {
        maximum_noise_error = std::max(maximum_noise_error, error);
        ++noise_checks;
    } else {
        maximum_wave_error = std::max(maximum_wave_error, error);
        ++wave_checks;
    }
}

struct Physical {
    std::vector<ConversionChannel> channels;
    SMatrix direct_admittance, conjugate_admittance;
};

struct PhysicalWaves {
    Vector voltage, current;
};

// Independent real Gaussian elimination, using only V/I device equations and
// physical Thevenin boundaries/wires. No RFModel wave solver or transforms.
static PhysicalWaves
physical_solve(const Physical &device,
               const Vector &source_impedance,
               const Vector &source_voltage,
               const Vector &current_offset,
               const std::vector<std::pair<std::size_t, std::size_t>> &wires = {}) {
    const auto n = device.channels.size();
    std::vector<std::size_t> offsets(n);
    std::size_t real_dimension = 0;
    for (std::size_t i = 0; i < n; ++i) {
        offsets[i] = real_dimension;
        real_dimension += device.channels[i].bin ? 2 : 1;
    }
    const auto dimension = 2 * real_dimension;
    const auto residual = [&](const std::vector<double> &variables) {
        Vector voltage(n), current(n), equations(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto k = offsets[i];
            voltage[i] = {variables[k], device.channels[i].bin ? variables[k + 1] : 0.};
            current[i] = {variables[real_dimension + k],
                          device.channels[i].bin ? variables[real_dimension + k + 1] : 0.};
        }
        for (std::size_t i = 0; i < n; ++i) {
            equations[i] = current[i] - current_offset[i];
            for (std::size_t j = 0; j < n; ++j) {
                equations[i] -= device.direct_admittance(i, j) * voltage[j] +
                                device.conjugate_admittance(i, j) * std::conj(voltage[j]);
            }
            equations[n + i] = voltage[i] + source_impedance[i] * current[i] - source_voltage[i];
        }
        for (const auto &wire : wires) {
            equations[n + wire.first] = voltage[wire.first] - voltage[wire.second];
            equations[n + wire.second] = current[wire.first] + current[wire.second];
        }
        std::vector<double> result(dimension);
        for (std::size_t i = 0; i < 2 * n; ++i) {
            const auto channel = i % n;
            const auto k = offsets[channel] + (i < n ? 0 : real_dimension);
            result[k] = equations[i].real();
            if (device.channels[channel].bin) {
                result[k + 1] = equations[i].imag();
            }
        }
        return result;
    };
    std::vector<double> basis(dimension);
    const auto constant = residual(basis);
    std::vector<std::vector<double>> augmented(dimension, std::vector<double>(dimension + 1));
    for (std::size_t column = 0; column < dimension; ++column) {
        basis[column] = 1.;
        const auto value = residual(basis);
        basis[column] = 0.;
        for (std::size_t row = 0; row < dimension; ++row) {
            augmented[row][column] = value[row] - constant[row];
        }
    }
    for (std::size_t row = 0; row < dimension; ++row) {
        augmented[row][dimension] = -constant[row];
    }
    for (std::size_t column = 0; column < dimension; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < dimension; ++row) {
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) {
                pivot = row;
            }
        }
        require(std::abs(augmented[pivot][column]) > 1e-13, "independent physical system singular");
        std::swap(augmented[pivot], augmented[column]);
        const auto divisor = augmented[column][column];
        for (std::size_t j = column; j <= dimension; ++j) {
            augmented[column][j] /= divisor;
        }
        for (std::size_t row = 0; row < dimension; ++row) {
            if (row == column) {
                continue;
            }
            const auto factor = augmented[row][column];
            for (std::size_t j = column; j <= dimension; ++j) {
                augmented[row][j] -= factor * augmented[column][j];
            }
        }
    }
    PhysicalWaves result{Vector(n), Vector(n)};
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = offsets[i];
        result.voltage[i] = {augmented[k][dimension],
                             device.channels[i].bin ? augmented[k + 1][dimension] : 0.};
        result.current[i] = {augmented[real_dimension + k][dimension],
                             device.channels[i].bin ? augmented[real_dimension + k + 1][dimension]
                                                    : 0.};
    }
    return result;
}

static Vector incident(const PhysicalWaves &physical, const Vector &references) {
    Vector result(references.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = (physical.voltage[i] + references[i] * physical.current[i]) /
                    (2. * std::sqrt(references[i].real()));
    }
    return result;
}

static Vector outgoing(const PhysicalWaves &physical, const Vector &references) {
    Vector result(references.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = (physical.voltage[i] - std::conj(references[i]) * physical.current[i]) /
                    (2. * std::sqrt(references[i].real()));
    }
    return result;
}

static Physical physical_device(double pump = 1.) {
    const std::vector<ConversionChannel> channels{{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}, {1, 2}};
    auto a = conversion_detail::zero(channels.size()), b = a;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        a(i, i) = channels[i].bin ? .04 : .02;
        b(i, i) = channels[i].bin ? 0. : .02;
        a(i, i ^ 1) = channels[i].bin ? -.01 : -.005;
        b(i, i ^ 1) = channels[i].bin ? 0. : -.005;
    }
    a(5, 2) = pump * Complex{.004, .002};
    b(5, 2) = pump * Complex{-.001, .0005};
    a(2, 5) = pump * Complex{.003, -.001};
    b(2, 5) = pump * Complex{.0015, -.0002};
    a(3, 0) = b(3, 0) = pump * Complex{.0008, .0003};
    a(1, 4) = pump * Complex{.001, .0004};
    b(1, 4) = std::conj(a(1, 4));
    return {channels, a, b};
}

static FrequencyConversionModel local_model(const Physical &physical, const Vector &references) {
    const auto n = physical.channels.size();
    auto a = conversion_detail::zero(n), b = a;
    for (std::size_t j = 0; j < n; ++j) {
        Vector source(n);
        source[j] = 2. * std::sqrt(references[j].real());
        const auto real =
            outgoing(physical_solve(physical, references, source, Vector(n)), references);
        Vector imag(n);
        if (physical.channels[j].bin) {
            source[j] *= Complex{0., 1.};
            imag = outgoing(physical_solve(physical, references, source, Vector(n)), references);
        }
        for (std::size_t i = 0; i < n; ++i) {
            auto r = real[i], q = imag[i];
            if (!physical.channels[i].bin) {
                require(std::abs(r.imag()) < 1e-13 && std::abs(q.imag()) < 1e-13,
                        "reference DC is not real");
                r = r.real();
                q = q.real();
            }
            a(i, j) = (r - Complex{0., 1.} * q) * .5;
            b(i, j) = (r + Complex{0., 1.} * q) * .5;
        }
    }
    return {1e9, physical.channels, a, b, references};
}

static Vector
local_offset(const Physical &physical, const Vector &references, const Vector &current) {
    auto result = outgoing(physical_solve(physical, references, Vector(references.size()), current),
                           references);
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!physical.channels[i].bin) {
            require(std::abs(result[i].imag()) < 1e-13, "offset DC is not real");
            result[i] = result[i].real();
        }
    }
    return result;
}

static void add_noise(ConversionNoise &noise, const Vector &first, const Vector &second) {
    for (std::size_t i = 0; i < first.size(); ++i) {
        for (std::size_t j = 0; j < first.size(); ++j) {
            noise.covariance(i, j) += first[i] * std::conj(second[j]);
            noise.complementary(i, j) += first[i] * second[j];
        }
    }
}

static void compare_noise(const ConversionNoise &actual, const ConversionNoise &expected) {
    require(actual.covariance.ports == expected.covariance.ports, "noise dimensions");
    for (std::size_t i = 0; i < actual.covariance.values.size(); ++i) {
        close_value(actual.covariance.values[i], expected.covariance.values[i], true);
        close_value(actual.complementary.values[i], expected.complementary.values[i], true);
    }
}

static Physical combine(const Physical &first, const Physical &second) {
    Physical result{first.channels, conversion_detail::zero(12), conversion_detail::zero(12)};
    for (auto channel : second.channels) {
        channel.port += 2;
        result.channels.push_back(channel);
    }
    for (std::size_t i = 0; i < 6; ++i) {
        for (std::size_t j = 0; j < 6; ++j) {
            result.direct_admittance(i, j) = first.direct_admittance(i, j);
            result.conjugate_admittance(i, j) = first.conjugate_admittance(i, j);
            result.direct_admittance(6 + i, 6 + j) = second.direct_admittance(i, j);
            result.conjugate_admittance(6 + i, 6 + j) = second.conjugate_admittance(i, j);
        }
    }
    return result;
}

static void test_physical_network() {
    const auto first = physical_device(), second = physical_device(-.7),
               combined = combine(first, second);
    const Vector z1{25., 80., {31., 12.}, {73., -21.}, {19., 8.}, {110., 31.}};
    const Vector z2{43., 91., {67., -27.}, {22., 9.}, {120., 37.}, {39., -12.}};
    Vector references = z1;
    references.insert(references.end(), z2.begin(), z2.end());
    Vector impedance(12, 50.), voltage(12), current(12);
    voltage[0] = .3;
    voltage[2] = {.2, -.1};
    voltage[4] = {-.1, .13};
    voltage[7] = -.2;
    voltage[9] = {.11, .07};
    voltage[11] = {-.04, -.09};
    impedance[2] = {62., 7.};
    impedance[9] = {38., -11.};
    current[0] = .0003;
    current[3] = {.0002, -.0001};
    current[7] = -.0002;
    current[10] = {-.00015, .00008};
    const std::vector<std::pair<std::size_t, std::size_t>> wires{{1, 6}, {3, 8}, {5, 10}};
    auto m1 = local_model(first, z1), m2 = local_model(second, z2);
    ConversionDevice d1{m1, Vector(6), Vector(6), m1.zero_noise(), m1.zero_noise()};
    ConversionDevice d2{m2, Vector(6), Vector(6), m2.zero_noise(), m2.zero_noise()};
    for (const auto i : {0u, 2u, 4u, 7u, 9u, 11u}) {
        auto &d = i < 6 ? d1 : d2;
        const auto j = i % 6;
        d.reflection[j] =
            (impedance[i] - references[i]) / (impedance[i] + std::conj(references[i]));
        d.source[j] = std::sqrt(references[i].real()) * voltage[i] /
                      (impedance[i] + std::conj(references[i]));
    }
    Vector c1(current.begin(), current.begin() + 6), c2(current.begin() + 6, current.end());
    auto offsets = local_offset(first, z1, c1);
    const auto offsets2 = local_offset(second, z2, c2);
    offsets.insert(offsets.end(), offsets2.begin(), offsets2.end());
    // Real independent Gaussian basis amplitudes produce arbitrary valid C/P,
    // including DC/RF correlations and improper quadrature statistics.
    std::vector<Vector> source_bases(2, Vector(12)), current_bases(4, Vector(12));
    source_bases[0][0] = 1e-10;
    source_bases[0][4] = {2e-10, -1e-10};
    source_bases[0][9] = {-.7e-10, .8e-10};
    source_bases[1][2] = {-.8e-10, 1.3e-10};
    source_bases[1][7] = -1.1e-10;
    source_bases[1][11] = {.5e-10, .4e-10};
    current_bases[0][0] = 1e-11;
    current_bases[0][3] = {-.3e-11, .7e-11};
    current_bases[1][2] = {.8e-11, .3e-11};
    current_bases[1][5] = {-.6e-11, .9e-11};
    current_bases[2][7] = -1.2e-11;
    current_bases[2][10] = {.2e-11, -.7e-11};
    current_bases[3][8] = {1.1e-11, .3e-11};
    current_bases[3][11] = {-.4e-11, -.2e-11};
    ConversionNoise source_noise{conversion_detail::zero(12), conversion_detail::zero(12)};
    for (const auto &basis : source_bases) {
        Vector wave(12);
        for (std::size_t i = 0; i < 12; ++i) {
            wave[i] = std::sqrt(references[i].real()) * basis[i] /
                      (impedance[i] + std::conj(references[i]));
        }
        add_noise(source_noise, wave, wave);
    }
    for (std::size_t k = 0; k < current_bases.size(); ++k) {
        const auto start = k < 2 ? 0 : 6;
        const Vector local(current_bases[k].begin() + start, current_bases[k].begin() + start + 6);
        const auto wave = local_offset(k < 2 ? first : second, k < 2 ? z1 : z2, local);
        add_noise(k < 2 ? d1.intrinsic_noise : d2.intrinsic_noise, wave, wave);
    }
    FrequencyConversionNetwork network(1e9,
                                       75.); // Construction default is not a device coordinate.
    rejects<std::invalid_argument>([&] {
        network.add(d1);
    });
    require(network.channel_count() == 0, "rejected legacy add mutated network");
    network.add_power_waves(d1);
    network.add_power_waves(d2);
    require(network.reference_impedances_ohms() == references, "channel references reordered");
    network.connect(0, 1, 1, 0);
    const auto result = network.analyze(true, &source_noise, offsets);
    const auto physical = physical_solve(combined, impedance, voltage, current, wires);
    const auto expected_a = incident(physical, references),
               expected_b = outgoing(physical, references);
    for (std::size_t i = 0; i < 12; ++i) {
        close_value(result.incident[i], expected_a[i]);
        close_value(result.outgoing[i], expected_b[i]);
        const auto v =
            (std::conj(references[i]) * result.incident[i] + references[i] * result.outgoing[i]) /
            std::sqrt(references[i].real());
        const auto current_value =
            (result.incident[i] - result.outgoing[i]) / std::sqrt(references[i].real());
        close_value(v, physical.voltage[i]);
        close_value(current_value, physical.current[i]);
        close_value(std::norm(result.incident[i]) - std::norm(result.outgoing[i]),
                    (physical.voltage[i] * std::conj(physical.current[i])).real());
    }
    for (const auto &wire : wires) {
        close_value(physical.voltage[wire.first], physical.voltage[wire.second]);
        close_value(physical.current[wire.first], -physical.current[wire.second]);
    }
    ConversionNoise a_noise{conversion_detail::zero(12), conversion_detail::zero(12)};
    auto b_noise = a_noise, ab_noise = a_noise;
    for (std::size_t k = 0; k < source_bases.size() + current_bases.size(); ++k) {
        const auto noise_physical =
            k < source_bases.size()
                ? physical_solve(combined, impedance, source_bases[k], Vector(12), wires)
                : physical_solve(combined,
                                 impedance,
                                 Vector(12),
                                 current_bases[k - source_bases.size()],
                                 wires);
        const auto a = incident(noise_physical, references),
                   b = outgoing(noise_physical, references);
        add_noise(a_noise, a, a);
        add_noise(b_noise, b, b);
        add_noise(ab_noise, a, b);
    }
    compare_noise(result.incident_noise, a_noise);
    compare_noise(result.outgoing_noise, b_noise);
    compare_noise(result.incident_outgoing_noise, ab_noise);
    for (std::size_t i = 0; i < 12; ++i) {
        close_value(result.net_noise_into_device_w_per_hz[i],
                    a_noise.covariance(i, i) - b_noise.covariance(i, i),
                    true);
    }
    for (const auto &wire : wires) {
        close_value(result.net_noise_into_device_w_per_hz[wire.first] +
                        result.net_noise_into_device_w_per_hz[wire.second],
                    0.,
                    true);
    }
    // The independent noise outputs are nontrivial at both cross-frequency
    // covariance and complementary cross-device entries.
    require(std::abs(b_noise.complementary(2, 9)) > 1e-25, "improper-noise fixture became trivial");
}

static void test_coordinate_change() {
    const auto physical = physical_device();
    const Vector old_refs{50., 75., {31., 12.}, {73., -21.}, {19., 8.}, {110., 31.}};
    const auto old = local_model(physical, old_refs);
    const Vector norton{.0001, -.0002, {.0003, .0002}, {-.0001, .0001}, {}, {.0002, -.0003}};
    const Vector offset = local_offset(physical, old_refs, norton);
    const std::vector<Vector> noise_bases{
        {1e-11, -.4e-11, {.3e-11, .7e-11}, {}, {-.6e-11, .8e-11}, {}},
        {-.2e-11, .8e-11, {}, {.5e-11, -.9e-11}, {}, {.3e-11, .4e-11}}};
    auto old_noise = old.zero_noise();
    for (const auto &basis : noise_bases) {
        const auto wave = local_offset(physical, old_refs, basis);
        add_noise(old_noise, wave, wave);
    }
    for (const Vector target :
         {Vector(6, 50.), Vector{29., 120., {47., -6.}, {16., 23.}, {85., -36.}, {130., 18.}}}) {
        const auto changed = renormalize_power_waves(old, target);
        const auto expected = local_model(physical, target);
        const auto restored = renormalize_power_waves(changed.model, old_refs);
        for (std::size_t i = 0; i < expected.direct().values.size(); ++i) {
            close_value(changed.model.direct().values[i], expected.direct().values[i]);
            close_value(changed.model.conjugate().values[i], expected.conjugate().values[i]);
            close_value(restored.model.direct().values[i], old.direct().values[i]);
            close_value(restored.model.conjugate().values[i], old.conjugate().values[i]);
        }
        const auto converted_offset = renormalize_conversion_offset(changed, offset);
        const auto expected_offset = local_offset(physical, target, norton);
        for (std::size_t i = 0; i < 6; ++i) {
            close_value(converted_offset[i], expected_offset[i]);
        }
        auto expected_noise = old.zero_noise();
        for (const auto &basis : noise_bases) {
            const auto wave = local_offset(physical, target, basis);
            add_noise(expected_noise, wave, wave);
        }
        compare_noise(renormalize_conversion_noise(changed, old_noise), expected_noise);
        compare_noise(renormalize_conversion_noise(
                          restored, renormalize_conversion_noise(changed, old_noise)),
                      old_noise);
    }
    const auto identity = renormalize_power_waves(old, old_refs);
    require(identity.model.direct().values == old.direct().values &&
                identity.model.conjugate().values == old.conjugate().values,
            "same-coordinate change is not exact");
    require(identity.model.reference_impedances_ohms() == old_refs,
            "transformed references missing");
    rejects<std::invalid_argument>([&] {
        old.reference_ohms();
    });
    near(renormalize_power_waves(old, Vector(6, 75.)).model.reference_ohms(), 75.);
}

static void test_boundary_change() {
    const auto physical = physical_device();
    const auto old = local_model(physical, Vector(6, 50.));
    const Vector refs{31., 80., {31., 12.}, {73., -21.}, {19., 8.}, {110., 31.}};
    const Vector source{.01, -.02, {.03, -.01}, {.005, .008}, {-.01, .009}, {.02, -.003}};
    const Vector reflection{.1, -.2, {.15, .06}, {-.1, .03}, {}, {.08, -.11}};
    auto noise = old.zero_noise();
    const Vector basis{1e-10, -.5e-10, {.4e-10, -.6e-10}, {}, {}, {.7e-10, .2e-10}};
    add_noise(noise, basis, basis);
    const auto old_result = old.analyze(source, reflection, noise, old.zero_noise(), {}, true);
    const auto transform = renormalize_power_waves(old, refs);
    const auto boundary =
        renormalize_conversion_boundary(old, refs, {0, 1, 2, 3, 4, 5}, source, reflection, noise);
    const auto new_result = transform.model.analyze(
        boundary.source, boundary.reflection, boundary.source_noise, old.zero_noise(), {}, true);
    // Transform the independently reconstructed V/I, not the new model matrices.
    PhysicalWaves old_physical{Vector(6), Vector(6)};
    for (std::size_t i = 0; i < 6; ++i) {
        old_physical.voltage[i] =
            std::sqrt(50.) * (old_result.incident[i] + old_result.outgoing[i]);
        old_physical.current[i] =
            (old_result.incident[i] - old_result.outgoing[i]) / std::sqrt(50.);
    }
    const auto a = incident(old_physical, refs), b = outgoing(old_physical, refs);
    for (std::size_t i = 0; i < 6; ++i) {
        close_value(new_result.incident[i], a[i]);
        close_value(new_result.outgoing[i], b[i]);
        close_value(new_result.net_noise_into_device_w_per_hz[i],
                    old_result.net_noise_into_device_w_per_hz[i],
                    true);
    }
    Vector physical_impedance(6), physical_noise_voltage(6);
    for (std::size_t i = 0; i < 6; ++i) {
        physical_impedance[i] = 50. * (1. + reflection[i]) / (1. - reflection[i]);
        physical_noise_voltage[i] = 2. * std::sqrt(50.) * basis[i] / (1. - reflection[i]);
    }
    const auto physical_noise =
        physical_solve(physical, physical_impedance, physical_noise_voltage, Vector(6));
    const auto noise_a = incident(physical_noise, refs), noise_b = outgoing(physical_noise, refs);
    auto expected_a_noise = old.zero_noise(), expected_b_noise = old.zero_noise();
    auto expected_cross = old.zero_noise();
    add_noise(expected_a_noise, noise_a, noise_a);
    add_noise(expected_b_noise, noise_b, noise_b);
    add_noise(expected_cross, noise_a, noise_b);
    compare_noise(new_result.incident_noise, expected_a_noise);
    compare_noise(new_result.outgoing_noise, expected_b_noise);
    compare_noise(new_result.incident_outgoing_noise, expected_cross);
    auto empty =
        renormalize_conversion_boundary(old, refs, {}, Vector(6), Vector(6), old.zero_noise());
    require(empty.source == Vector(6) && empty.reflection == Vector(6),
            "internal boundaries acquired drives");
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_boundary(old, refs, {0, 0}, source, reflection, noise);
    });
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_boundary(old, refs, {6}, source, reflection, noise);
    });
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_boundary(old, refs, {0}, source, reflection, noise);
    });
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_boundary(old, refs, {}, Vector(6), Vector(6), noise);
    });
}

static void test_mixer_dc_coordinates() {
    const std::vector<ConversionChannel> channels{
        {0, 8}, {0, 10}, {0, 12}, {0, 0}, {1, 2}, {1, 18}, {1, 20}, {1, 22}, {1, 0}, {1, 10}};
    const auto original = ideal_mixer_conversion(1e8, channels, 10, -3., .4);
    const Vector source{{.01, .02}, {.007, -.003}, {-.004, .008}, .009, {}, {}, {}, {}, {}, {}};
    const Vector references{{31., 12.},
                            {73., -21.},
                            {19., 8.},
                            25.,
                            {110., 31.},
                            {43., -12.},
                            {22., 7.},
                            {91., -18.},
                            75.,
                            {66., 13.}};
    const auto changed = renormalize_power_waves(original, references);
    const auto boundary = renormalize_conversion_boundary(original,
                                                          references,
                                                          {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                                                          source,
                                                          Vector(10),
                                                          original.zero_noise());
    const auto result = changed.model.analyze(
        boundary.source, boundary.reflection, boundary.source_noise, original.zero_noise());
    const auto pump = std::polar(std::pow(10., -3. / 20.), .4);
    const Vector expected{{},
                          {},
                          {},
                          {},
                          pump * std::conj(source[0]) + std::conj(pump) * source[2],
                          pump * source[0],
                          pump * source[1],
                          pump * source[2],
                          std::sqrt(2.) * (std::conj(pump) * source[1]).real(),
                          std::sqrt(2.) * pump * source[3]};
    for (std::size_t i = 0; i < 10; ++i) {
        const auto z = references[i];
        const auto v =
            (std::conj(z) * result.incident[i] + z * result.outgoing[i]) / std::sqrt(z.real());
        const auto current = (result.incident[i] - result.outgoing[i]) / std::sqrt(z.real());
        close_value((v + 50. * current) / (2. * std::sqrt(50.)), source[i]);
        close_value((v - 50. * current) / (2. * std::sqrt(50.)), expected[i]);
    }
    // Separate DC A/B columns are not unique. A round trip must preserve the
    // physical relation, including sqrt(2) DC/RF normalization, not its spelling.
    const auto restored = renormalize_power_waves(changed.model, Vector(10, 50.));
    const auto replay =
        restored.model.analyze(source, Vector(10), original.zero_noise(), original.zero_noise());
    for (std::size_t i = 0; i < 10; ++i) {
        close_value(replay.outgoing[i], expected[i]);
    }
}

static void test_wide_reference_precision() {
    const FrequencyConversionModel model(1., {{0, 1}}, {1, {0.}}, {1, {0.}}, 1.);
    auto noise = model.zero_noise();
    noise.covariance(0, 0) = 1.;
    noise.complementary(0, 0) = {.3, .2};
    for (const Complex target : Vector{1e-24, {1e-24, 3e-25}, 1e-12, 1e12, 1e24, {1e24, 3e23}}) {
        // A physical 1 ohm matched device/source. Derive the transfer from its
        // Thevenin/Norton relation; no subtraction of nearly equal wave terms.
        const auto device_transfer = 2. * std::sqrt(target.real()) / (1. + target);
        const auto source_transfer = 2. * std::sqrt(target.real()) / (1. + std::conj(target));
        const auto changed = renormalize_power_waves(model, {target});
        relative_value(changed.noise_direct(0, 0), device_transfer);
        require(std::abs(changed.noise_conjugate(0, 0)) < 1e-12 * std::abs(device_transfer),
                "ordinary reference transform acquired a conjugate transfer");
        relative_value(renormalize_conversion_offset(changed, {1.})[0], device_transfer);
        const auto transformed = renormalize_conversion_noise(changed, noise);
        relative_value(transformed.covariance(0, 0), std::norm(device_transfer));
        relative_value(transformed.complementary(0, 0),
                       device_transfer * device_transfer * noise.complementary(0, 0));
        const auto boundary =
            renormalize_conversion_boundary(model, {target}, {0}, {1.}, {0.}, noise);
        relative_value(boundary.source[0], source_transfer);
        relative_value(boundary.source_noise.covariance(0, 0), std::norm(source_transfer));
        relative_value(boundary.source_noise.complementary(0, 0),
                       source_transfer * source_transfer * noise.complementary(0, 0));
        const auto linear = renormalize_power_waves(SMatrix{1, {0.}}, {1.}, {target});
        relative_value(linear.noise_transfer(0, 0), device_transfer);
        const auto linear_noise = renormalize_noise(SMatrix{1, {0.}}, {{1, {1.}}}, {1.}, {target});
        relative_value(linear_noise.watts_per_hz(0, 0), std::norm(device_transfer));
    }
}

// Scale every assertion to its physical quantity, including small correlations.
// In particular, thermal noise must never inherit a unit-sized absolute floor.
static void fixture_value(Complex actual, Complex expected, double scale, const char *message) {
    require(std::isfinite(scale) && scale > 0., "invalid independent fixture scale");
    const double error = std::abs(actual - expected) / scale;
    require(std::isfinite(error) && error < 3e-12, message);
    maximum_boundary_fixture_scaled_error = std::max(maximum_boundary_fixture_scaled_error, error);
    ++boundary_fixture_checks;
}

static void test_partial_boundary_thevenin() {
    const std::vector<ConversionChannel> channels{{0, 1}, {0, 2}, {1, 3}};
    const Vector old_refs{50., {30., 9.}, {81., -4.}};
    const Vector new_refs{{21., 7.}, {93., -15.}, {64., 12.}};
    const Vector impedance{{35., 4.}, {}, {74., -5.}};
    const Vector voltage{{.4, .2}, {}, {0., -.2}};
    // Independent real latent Thevenin voltages produce both C and nonzero P.
    const std::vector<Vector> bases{{{.2, .05}, {}, {0., -.1}}, {{0., .03}, {}, {.04, -.06}}};
    const FrequencyConversionModel model(1., channels, {3, Vector(9)}, {3, Vector(9)}, old_refs);
    const auto physical_boundary = [&](const Vector &refs) {
        ConversionPowerWaveBoundary result{Vector(3), Vector(3), model.zero_noise()};
        Vector transfer(3);
        for (const auto i : {0u, 2u}) {
            transfer[i] = std::sqrt(refs[i].real()) / (impedance[i] + std::conj(refs[i]));
            result.source[i] = transfer[i] * voltage[i];
            result.reflection[i] = (impedance[i] - refs[i]) / (impedance[i] + std::conj(refs[i]));
        }
        for (const auto &basis : bases) {
            Vector wave(3);
            for (const auto i : {0u, 2u}) {
                wave[i] = transfer[i] * basis[i];
            }
            add_noise(result.source_noise, wave, wave);
        }
        return result;
    };
    const auto original = physical_boundary(old_refs), saved = original;
    const auto check = [&](const ConversionPowerWaveBoundary &actual,
                           const ConversionPowerWaveBoundary &expected) {
        for (const auto i : {0u, 2u}) {
            fixture_value(actual.source[i],
                          expected.source[i],
                          std::abs(expected.source[i]),
                          "partial boundary Thevenin source");
            fixture_value(actual.reflection[i],
                          expected.reflection[i],
                          1.,
                          "partial boundary Thevenin reflection");
            for (const auto j : {0u, 2u}) {
                const double scale = std::sqrt(expected.source_noise.covariance(i, i).real() *
                                               expected.source_noise.covariance(j, j).real());
                fixture_value(actual.source_noise.covariance(i, j),
                              expected.source_noise.covariance(i, j),
                              scale,
                              "partial boundary Thevenin covariance");
                fixture_value(actual.source_noise.complementary(i, j),
                              expected.source_noise.complementary(i, j),
                              scale,
                              "partial boundary Thevenin complementary covariance");
            }
        }
        require(actual.source[1] == Complex{} && actual.reflection[1] == Complex{},
                "partial boundary internal drive must stay exactly zero");
        for (std::size_t i = 0; i < 3; ++i) {
            require(actual.source_noise.covariance(1, i) == Complex{} &&
                        actual.source_noise.covariance(i, 1) == Complex{} &&
                        actual.source_noise.complementary(1, i) == Complex{} &&
                        actual.source_noise.complementary(i, 1) == Complex{},
                    "partial boundary internal noise rows/columns must stay exactly zero");
        }
        require(std::abs(actual.source_noise.covariance(0, 2)) > 1e-6 &&
                    std::abs(actual.source_noise.complementary(0, 2)) > 1e-6,
                "partial boundary fixture lost its cross-channel C/P");
    };
    const auto changed = renormalize_conversion_boundary(
        model, new_refs, {0, 2}, original.source, original.reflection, original.source_noise);
    check(changed, physical_boundary(new_refs));
    // Deliberately reverse selection order: indices, not list positions, define coordinates.
    const auto changed_model = renormalize_power_waves(model, new_refs).model;
    const auto restored = renormalize_conversion_boundary(
        changed_model, old_refs, {2, 0}, changed.source, changed.reflection, changed.source_noise);
    check(restored, original);
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_boundary(
            model, new_refs, {0}, original.source, original.reflection, original.source_noise);
    });
    auto bad_noise = original.source_noise;
    bad_noise.covariance(1, 1) = 1e-4;
    const auto saved_bad_noise = bad_noise;
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_boundary(
            model, new_refs, {0, 2}, original.source, original.reflection, bad_noise);
    });
    require(bad_noise.covariance.values == saved_bad_noise.covariance.values &&
                bad_noise.complementary.values == saved_bad_noise.complementary.values,
            "rejected partial boundary mutated invalid input noise");
    require(original.source == saved.source && original.reflection == saved.reflection &&
                original.source_noise.covariance.values == saved.source_noise.covariance.values &&
                original.source_noise.complementary.values ==
                    saved.source_noise.complementary.values &&
                model.reference_impedances_ohms() == old_refs &&
                model.direct().values == Vector(9) && model.conjugate().values == Vector(9),
            "partial boundary success/failure mutated caller data");
}

static void test_mismatched_extreme_references() {
    constexpr double resistance = 75.;
    const Complex old = 1., impedance{45., 3.}, voltage{.4, .2}, offset{-.02, .01};
    const Complex scattering = (resistance - std::conj(old)) / (resistance + old);
    const Complex gamma = (impedance - old) / (impedance + std::conj(old));
    const Complex source = voltage / (impedance + std::conj(old));
    const FrequencyConversionModel model(1., {{0, 1}}, {1, {scattering}}, {1, {0.}}, {old});
    const ConversionNoise intrinsic{{1, {2.}}, {1, {Complex{.3, .2}}}};
    const ConversionNoise source_noise{{1, {2.}}, {1, {Complex{0., .3}}}};
    for (const double scale : {1e-24, 1e-12, 1., 1e12, 1e24}) {
        for (const Complex reference : Vector{scale, scale * Complex{1., .3}}) {
            // I=V/R+J gives b=(R-conj(Z))/(R+Z)*a - R*sqrt(Re Z)/(R+Z)*J.
            // The fixed physical Norton offset/noise is thus scaled by this ratio.
            const auto device_transfer =
                std::sqrt(reference.real()) * (resistance + old) / (resistance + reference);
            const auto changed = renormalize_power_waves(model, {reference});
            fixture_value(changed.model.direct()(0, 0),
                          (resistance - std::conj(reference)) / (resistance + reference),
                          1.,
                          "mismatched extreme-reference scattering");
            relative_value(changed.noise_direct(0, 0), device_transfer);
            fixture_value(changed.noise_conjugate(0, 0),
                          0.,
                          std::abs(device_transfer),
                          "ordinary Norton noise acquired conjugate transfer");
            relative_value(renormalize_conversion_offset(changed, {offset})[0],
                           device_transfer * offset);
            const auto noise = renormalize_conversion_noise(changed, intrinsic);
            relative_value(noise.covariance(0, 0), 2. * std::norm(device_transfer));
            relative_value(noise.complementary(0, 0),
                           Complex{.3, .2} * device_transfer * device_transfer);
            // V+Zs*I=Vs gives a=Gamma*b+sqrt(Re Z)/(Zs+conj(Z))*Vs.
            const auto source_transfer = std::sqrt(reference.real()) *
                                         (impedance + std::conj(old)) /
                                         (impedance + std::conj(reference));
            const auto boundary = renormalize_conversion_boundary(
                model, {reference}, {0}, {source}, {gamma}, source_noise);
            relative_value(boundary.source[0],
                           voltage * std::sqrt(reference.real()) /
                               (impedance + std::conj(reference)));
            fixture_value(boundary.reflection[0],
                          (impedance - reference) / (impedance + std::conj(reference)),
                          1.,
                          "mismatched extreme-reference Thevenin reflection");
            relative_value(boundary.source_noise.covariance(0, 0), 2. * std::norm(source_transfer));
            relative_value(boundary.source_noise.complementary(0, 0),
                           Complex{0., .3} * source_transfer * source_transfer);
        }
    }
    require(model.reference_impedances_ohms() == Vector{old} &&
                model.direct().values == Vector{scattering} &&
                model.conjugate().values == Vector{0.} &&
                intrinsic.covariance.values == Vector{2.} &&
                intrinsic.complementary.values == Vector{Complex{.3, .2}} &&
                source_noise.covariance.values == Vector{2.} &&
                source_noise.complementary.values == Vector{Complex{0., .3}},
            "extreme-reference changes mutated physical input data");
}

static void test_unequal_dc_linear_lift() {
    constexpr double resistance = 25., temperature = 290., kt = 1.380649e-23 * temperature;
    const IdealRLCModel resistor("thermal series resistor",
                                 IdealElement::Resistor,
                                 LumpedConnection::SeriesImpedance,
                                 resistance);
    const std::vector<Vector> refs{{31., 73.}, {{19., 8.}, {110., 31.}}};
    std::vector<SMatrix> scattering;
    std::vector<NoiseCorrelation> noise;
    for (std::size_t k = 0; k < refs.size(); ++k) {
        const auto original = resistor.s_parameters(static_cast<double>(k) * 1e9);
        scattering.push_back(renormalize_power_waves(original, {50., 50.}, refs[k]).scattering);
        noise.push_back(renormalize_noise(
            original, passive_thermal_noise(original, temperature), {50., 50.}, refs[k]));
    }
    auto device = lift_linear_conversion(1e9, {0, 1}, scattering, noise, refs);
    const Vector sources{.02, -.01, {.03, -.02}, {-.01, .04}};
    device.source = sources;
    FrequencyConversionNetwork network(1e9);
    network.add_power_waves(device);
    const auto result = network.analyze(true);
    require(network.reference_impedances_ohms() ==
                Vector{refs[0][0], refs[0][1], refs[1][0], refs[1][1]},
            "DC/RF linear lift reference order");
    const auto &channels = device.model.channels();
    for (std::size_t i = 0; i < 4; ++i) {
        require(channels[i].bin == static_cast<int>(i / 2) && channels[i].port == i % 2,
                "DC/RF linear lift is not frequency-major");
        Complex expected_wave{}, expected_self_noise{};
        for (std::size_t j = 0; j < 4; ++j) {
            Complex s{}, c{}, p{};
            if (i / 2 == j / 2) {
                const auto &z = refs[i / 2];
                const auto denominator = resistance + z[0] + z[1];
                const auto ri = z[i % 2].real(), rj = z[j % 2].real();
                s = i == j ? 1. - 2. * ri / denominator : 2. * std::sqrt(ri * rj) / denominator;
                // A series Johnson voltage has density 4*k*T*R. KCL makes its
                // two outgoing wave coefficients opposite in sign. This oracle
                // does not call passive_thermal_noise, renormalize, or a wave solve.
                c = (i == j ? 1. : -1.) * 4. * kt * resistance * std::sqrt(ri * rj) /
                    std::norm(denominator);
                p = i < 2 ? c : Complex{}; // DC is real; each RF sample is proper.
                if (i == j) {
                    expected_self_noise = c;
                }
                fixture_value(result.outgoing_noise.covariance(i, j),
                              c,
                              kt,
                              "unequal DC/RF lift full thermal covariance");
                fixture_value(result.outgoing_noise.complementary(i, j),
                              p,
                              kt,
                              "unequal DC/RF lift full thermal complementary covariance");
                fixture_value(device.intrinsic_noise.covariance(i, j),
                              c,
                              kt,
                              "unequal DC/RF intrinsic covariance");
                fixture_value(device.intrinsic_noise.complementary(i, j),
                              p,
                              kt,
                              "unequal DC/RF intrinsic complementary covariance");
            } else {
                require(result.outgoing_noise.covariance(i, j) == Complex{} &&
                            result.outgoing_noise.complementary(i, j) == Complex{} &&
                            device.intrinsic_noise.covariance(i, j) == Complex{} &&
                            device.intrinsic_noise.complementary(i, j) == Complex{},
                        "independent DC/RF frequency blocks became correlated");
            }
            fixture_value(device.model.direct()(i, j), s, 1., "unequal DC/RF scattering");
            require(device.model.conjugate()(i, j) == Complex{}, "linear lift became conjugate");
            expected_wave += s * sources[j];
            require(result.incident_noise.covariance(i, j) == Complex{} &&
                        result.incident_noise.complementary(i, j) == Complex{} &&
                        result.incident_outgoing_noise.covariance(i, j) == Complex{} &&
                        result.incident_outgoing_noise.complementary(i, j) == Complex{},
                    "cold matched local boundaries acquired incident or cross noise");
        }
        fixture_value(result.incident[i], sources[i], std::abs(sources[i]), "linear lift incident");
        fixture_value(result.outgoing[i], expected_wave, .1, "linear lift physical outgoing");
        fixture_value(result.net_noise_into_device_w_per_hz[i],
                      -expected_self_noise,
                      kt,
                      "linear lift net thermal noise");
    }
    // Invalid DC coordinates reject without altering the successful sample inputs.
    const auto saved_scattering = scattering;
    const auto saved_noise = noise;
    auto invalid_refs = refs;
    invalid_refs[0][1] += Complex{0., 1.};
    rejects<std::invalid_argument>([&] {
        lift_linear_conversion(1e9, {0, 1}, scattering, noise, invalid_refs);
    });
    for (std::size_t k = 0; k < refs.size(); ++k) {
        require(scattering[k].values == saved_scattering[k].values &&
                    noise[k].watts_per_hz.values == saved_noise[k].watts_per_hz.values,
                "rejected complex DC lift mutated input samples");
    }
}

static void test_linear_lift_and_failures() {
    const SMatrix original{2, {.2, .6, .6, .2}};
    const Vector refs{{31., 12.}, {73., -21.}};
    const auto noise = passive_thermal_noise(original, 290.);
    const auto changed = renormalize_power_waves(original, {50., 50.}, refs);
    const auto changed_noise = renormalize_noise(original, noise, {50., 50.}, refs);
    auto device = lift_linear_conversion(
        1e9, {1}, {changed.scattering}, {changed_noise}, std::vector<Vector>{refs});
    const auto common = lift_linear_conversion(1e9, {1}, {original}, {noise});
    const auto boundary = renormalize_conversion_boundary(
        common.model, refs, {0, 1}, Vector(2), Vector(2), common.source_noise);
    device.reflection = boundary.reflection;
    FrequencyConversionNetwork mixed(1e9), ordinary(1e9);
    mixed.add_power_waves(device);
    ordinary.add(common);
    close_value(mixed.reference_noise_analysis({0}, {0}, 1).noise_factor,
                ordinary.reference_noise_analysis({0}, {0}, 1).noise_factor);
    // A DC sample with real references retains P=C; RF stays proper.
    const auto lifted = lift_linear_conversion(1e9,
                                               {0, 1},
                                               {original, changed.scattering},
                                               {noise, changed_noise},
                                               std::vector<Vector>{{50., 50.}, refs});
    require(lifted.model.reference_impedances_ohms() == Vector({50., 50., refs[0], refs[1]}),
            "lift reference order");
    lifted.model.validate_noise(lifted.intrinsic_noise);
    near(lifted.intrinsic_noise.complementary(0, 0), noise.watts_per_hz(0, 0));
    require(lifted.intrinsic_noise.complementary(2, 2) == Complex{}, "RF lift became improper");
    FrequencyConversionModel through(1., {{0, 1}, {1, 1}}, {2, {0., 1., 1., 0.}}, {2, Vector(4)});
    const auto new_through = renormalize_power_waves(through, {25., 100.});
    near(new_through.model.direct()(1, 0), .8);
    near(new_through.model.direct()(0, 0), .6);
    require(new_through.model.conjugate().values == Vector(4),
            "ordinary thru acquired conjugate terms");
    const auto check = [&](const Vector &target) {
        renormalize_power_waves(through, target);
    };
    rejects<std::invalid_argument>([&] {
        check({50.});
    });
    rejects<std::invalid_argument>([&] {
        check({0., 50.});
    });
    rejects<std::invalid_argument>([&] {
        check({{-1., 2.}, 50.});
    });
    rejects<std::invalid_argument>([&] {
        check({{50., std::numeric_limits<double>::infinity()}, 50.});
    });
    rejects<std::invalid_argument>([&] {
        FrequencyConversionModel bad(1., {{0, 0}}, {1, {0.}}, {1, {0.}}, Vector{{50., 1.}});
    });
    rejects<std::invalid_argument>([&] {
        lift_linear_conversion(1., {0}, {original}, {noise}, std::vector<Vector>{refs});
    });
    rejects<std::invalid_argument>([&] {
        lift_linear_conversion(1., {1}, {original}, {noise}, std::vector<Vector>{});
    });
    rejects<std::invalid_argument>([&] {
        lift_linear_conversion(1., {1}, {{999999999, {}}}, {noise});
    });
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_offset(new_through, {0.});
    });
    rejects<std::invalid_argument>([&] {
        renormalize_conversion_noise(new_through,
                                     {conversion_detail::zero(1), conversion_detail::zero(1)});
    });
    rejects<std::domain_error>([&] {
        FrequencyConversionModel pole(1., {{0, 1}}, {1, {1.25}}, {1, {0.}}, 1.);
        renormalize_power_waves(pole, {9.});
    });
    rejects<std::domain_error>([&] {
        FrequencyConversionModel model(1., {{0, 1}}, {1, {0.}}, {1, {0.}}, 1.);
        renormalize_conversion_boundary(model, {4.}, {0}, {1.}, {5. / 3.}, model.zero_noise());
    });
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point({device}, {}, {}, {}, {});
    });
}

void test_boundary_relative_singularity() {
    for (const auto scale : {1e-24, 1e-12, 1., 1e12, 1e24}) {
        FrequencyConversionModel model(1., {{0, 1}}, {1, {0.}}, {1, {0.}}, 50. * scale);
        const ConversionNoise noise{{1, {2.}}, {1, {Complex(.3, .2)}}};
        rejects<std::domain_error>([&] {
            renormalize_conversion_boundary(
                model, {100. * scale}, {0}, {Complex(.4, .2)}, {3.}, noise);
        });
        // A nearby resolvable active boundary remains valid, at every scale.
        const double gamma = 3. + 1e-8;
        const auto changed = renormalize_conversion_boundary(
            model, {100. * scale}, {0}, {Complex(.4, .2)}, {gamma}, noise);
        const double transfer = 2. * std::sqrt(2.) / (3. - gamma);
        require(std::abs(changed.source[0] / (Complex(.4, .2) * transfer) - 1.) < 2e-7,
                "near-pole boundary source");
        require(std::abs(changed.source_noise.covariance(0, 0).real() / (2. * transfer * transfer) -
                         1.) < 4e-7,
                "near-pole boundary covariance");
        require(std::abs(changed.source_noise.complementary(0, 0) /
                             (Complex(.3, .2) * transfer * transfer) -
                         1.) < 4e-7,
                "near-pole boundary complementary");
    }
}

int main() {
    try {
        test_physical_network();
        test_coordinate_change();
        test_boundary_change();
        test_mixer_dc_coordinates();
        test_wide_reference_precision();
        test_partial_boundary_thevenin();
        test_mismatched_extreme_references();
        test_unequal_dc_linear_lift();
        test_linear_lift_and_failures();
        test_boundary_relative_singularity();
        std::cout << std::setprecision(17) << "{\"passed\":true,\"wave_checks\":" << wave_checks
                  << ",\"noise_checks\":" << noise_checks
                  << ",\"maximum_wave_absolute_error\":" << maximum_wave_error
                  << ",\"maximum_noise_absolute_error_w_per_hz\":" << maximum_noise_error
                  << ",\"wide_reference_checks\":" << wide_reference_checks
                  << ",\"maximum_wide_reference_relative_error\":"
                  << maximum_wide_reference_relative_error
                  << ",\"boundary_fixture_checks\":" << boundary_fixture_checks
                  << ",\"maximum_boundary_fixture_scaled_error\":"
                  << maximum_boundary_fixture_scaled_error << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
