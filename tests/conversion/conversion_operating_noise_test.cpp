#include "rfmodel/conversion/conversion_simulation.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <type_traits>

using namespace rfmodel;

namespace {
using Vector = std::vector<Complex>;
using Channels = std::vector<ConversionChannel>;
using Devices = std::vector<ConversionDevice>;
using Wires = std::vector<ConversionPortConnection>;
using Laws = std::vector<ConversionNonlinearDevice>;
using Providers = std::vector<ConversionIntrinsicNoiseProvider>;
using OldSolve = ConversionOperatingResult (*)(const Devices &,
                                               const Wires &,
                                               const Laws &,
                                               const Vector &,
                                               const ConversionOperatingOptions &,
                                               bool,
                                               const ConversionNoise *,
                                               const Vector &);
static_assert(std::is_same<decltype(&solve_conversion_operating_point), OldSolve>::value,
              "the legacy solver address must remain unambiguous and unchanged");
static_assert(
    std::is_same<decltype(&solve_conversion_operating_point_power_waves), OldSolve>::value,
    "the power-wave solver address must remain unambiguous and unchanged");

std::size_t checks = 0;
double max_noise_error = 0.;

SMatrix zero(std::size_t n) {
    return {n, Vector(n * n)};
}

void close(Complex actual,
           Complex expected,
           const char *label,
           double absolute = 2e-11,
           double relative = 2e-9) {
    const double error = std::abs(actual - expected);
    if (!std::isfinite(error) || error > absolute + relative * std::abs(expected)) {
        std::ostringstream message;
        message << std::setprecision(17) << label << ": actual=" << actual
                << ", expected=" << expected << ", error=" << error;
        throw std::runtime_error(message.str());
    }
    ++checks;
}

void noise_close(Complex actual, Complex expected, const char *label) {
    close(actual, expected, label, 3e-29, 2e-8);
    max_noise_error = std::max(max_noise_error, std::abs(actual - expected));
}

ConversionDevice empty_device(Channels channels, Vector references = {}) {
    const auto n = channels.size();
    if (references.empty()) {
        references.assign(n, 50.);
    }
    FrequencyConversionModel model(1., std::move(channels), zero(n), zero(n), references);
    return {model, Vector(n), Vector(n), model.zero_noise(), model.zero_noise()};
}

// Columns are independent real unit-variance physical drivers. This builds C/P
// directly, without any RFModel factorization, noise transform, or network solve.
ConversionNoise covariance(const std::vector<Vector> &columns, std::size_t n) {
    ConversionNoise result{zero(n), zero(n)};
    for (const auto &column : columns) {
        require(column.size() == n, "oracle driver dimension differs");
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                result.covariance(i, j) += column[i] * std::conj(column[j]);
                result.complementary(i, j) += column[i] * column[j];
            }
        }
    }
    return result;
}

void check_noise(const ConversionResult &actual,
                 const std::vector<Vector> &a,
                 const std::vector<Vector> &b) {
    const auto n = actual.incident.size();
    require(a.size() == b.size(), "oracle driver counts differ");
    const auto aa = covariance(a, n), bb = covariance(b, n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            Complex c{}, p{};
            for (std::size_t k = 0; k < a.size(); ++k) {
                c += a[k][i] * std::conj(b[k][j]);
                p += a[k][i] * b[k][j];
            }
            noise_close(actual.incident_noise.covariance(i, j), aa.covariance(i, j), "incident C");
            noise_close(
                actual.incident_noise.complementary(i, j), aa.complementary(i, j), "incident P");
            noise_close(actual.outgoing_noise.covariance(i, j), bb.covariance(i, j), "outgoing C");
            noise_close(
                actual.outgoing_noise.complementary(i, j), bb.complementary(i, j), "outgoing P");
            noise_close(
                actual.incident_outgoing_noise.covariance(i, j), c, "incident-first cross C");
            noise_close(
                actual.incident_outgoing_noise.complementary(i, j), p, "incident-first cross P");
        }
        noise_close(actual.net_noise_into_device_w_per_hz[i],
                    aa.covariance(i, i) - bb.covariance(i, i),
                    "net noise");
    }
}

ConversionIntrinsicNoiseSnapshot snapshot(const ConversionAcceptedState &state,
                                          ConversionNoise noise) {
    return {state.jacobian.spacing_hz(),
            state.jacobian.channels(),
            state.jacobian.reference_impedances_ohms(),
            std::move(noise)};
}

ConversionOperatingOptions tight_options() {
    ConversionOperatingOptions options;
    options.relative_tolerance = 1e-12;
    options.absolute_tolerance = 1e-14;
    return options;
}

double scalar_root(double linear, double cubic, double forcing) {
    double lower = 0., upper = std::max(1., forcing / linear);
    for (int k = 0; k < 160; ++k) {
        const double middle = .5 * (lower + upper);
        if (linear * middle + cubic * middle * middle * middle < forcing) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    return .5 * (lower + upper);
}

void cubic_accepted_noise() {
    constexpr double gamma = 1., forcing = 1., fixed = .13;
    const double root = scalar_root(1., gamma, forcing + gamma * fixed);
    for (int kind = 0; kind < 3; ++kind) {
        const bool dc = kind == 2;
        auto device = empty_device({{3, dc ? 0 : 7}});
        device.source[0] = forcing;
        device.reflection[0] = gamma;
        const std::vector<Vector> source =
            dc          ? std::vector<Vector>{{2e-6}}
            : kind == 0 ? std::vector<Vector>{{2e-6}, {Complex{0., 2e-6}}}
                        : std::vector<Vector>{{Complex{2e-6, .7e-6}}, {Complex{-.4e-6, 1.1e-6}}};
        const std::vector<Vector> extra =
            dc ? std::vector<Vector>{{.8e-6}} : std::vector<Vector>{{Complex{.8e-6, -.3e-6}}};
        device.source_noise = covariance(source, 1);
        // A provider replaces this static sentinel, rather than adding to it.
        device.intrinsic_noise = covariance({{9e-4}}, 1);
        const auto additional = covariance(extra, 1);
        std::size_t law_calls = 0, noise_calls = 0;
        Vector seen_a, seen_b, seen_fixed;
        ConversionNonlinearDevice law{0, [&](const Vector &a) {
                                          ++law_calls;
                                          return ConversionLinearization{
                                              FrequencyConversionModel(1.,
                                                                       device.model.channels(),
                                                                       {1, {-2. * std::norm(a[0])}},
                                                                       {1, {-a[0] * a[0]}}),
                                              {-std::norm(a[0]) * a[0]}};
                                      }};
        const auto drivers = [&](double a, double b) {
            const double scale = 1e-6 * std::sqrt(1. + a * a + 2. * b * b + fixed * fixed);
            if (dc) {
                return std::vector<Vector>{{scale}};
            }
            if (kind == 0) {
                return std::vector<Vector>{{scale}, {Complex{0., scale}}};
            }
            return std::vector<Vector>{{Complex{scale, .35 * scale}},
                                       {Complex{-.2 * scale, .6 * scale}}};
        };
        ConversionIntrinsicNoiseProvider provider{
            0, [&](const ConversionAcceptedState &state) {
                ++noise_calls;
                seen_a = state.incident;
                seen_b = state.outgoing;
                seen_fixed = state.fixed_output_offset;
                close(state.jacobian.direct()(0, 0),
                      -2. * std::norm(state.incident[0]),
                      "provider accepted direct derivative");
                close(state.jacobian.conjugate()(0, 0),
                      -state.incident[0] * state.incident[0],
                      "provider accepted conjugate derivative");
                return snapshot(
                    state,
                    covariance(drivers(state.incident[0].real(), state.outgoing[0].real()), 1));
            }};
        const Devices input{device};
        const Vector initial{.01}, offset{fixed};
        const auto result = solve_conversion_operating_point_with_intrinsic_noise(
            input, {}, {law}, {provider}, initial, tight_options(), true, &additional, offset);
        require(result.iterations > 1 && result.backtracks > 0 && law_calls > result.iterations,
                "cubic fixture must exercise rejected Newton trials");
        require(noise_calls == 1, "provider ran before acceptance or more than once");
        require(seen_a == result.waves.incident && seen_b == result.waves.outgoing &&
                    seen_fixed == offset,
                "provider was not passed the exact accepted waves and original fixed offset");
        close(result.waves.incident[0], root, "independent bisection root");
        close(result.waves.outgoing[0], fixed - root * root * root, "true nonlinear output");
        require(std::abs(result.output_offset[0] - fixed) > .1,
                "fixture must distinguish fixed offset from affine offset");
        const double jx = -3. * root * root, jy = -root * root;
        const auto intrinsic = drivers(root, fixed - root * root * root);
        std::vector<Vector> a_columns, b_columns;
        for (const auto *group : {&source, &extra, &intrinsic}) {
            for (const auto &column : *group) {
                const auto q = column[0];
                const bool output_driver = group == &intrinsic;
                const Complex da{(output_driver ? gamma : 1.) * q.real() / (1. - gamma * jx),
                                 (output_driver ? gamma : 1.) * q.imag() / (1. - gamma * jy)};
                const Complex db{(output_driver ? 1. : jx) * q.real() / (1. - gamma * jx),
                                 (output_driver ? 1. : jy) * q.imag() / (1. - gamma * jy)};
                a_columns.push_back({da});
                b_columns.push_back({db});
            }
        }
        check_noise(result.waves, a_columns, b_columns);
        const auto accepted_noise = covariance(intrinsic, 1);
        noise_close(result.linearized_devices[0].intrinsic_noise.covariance(0, 0),
                    accepted_noise.covariance(0, 0),
                    "accepted intrinsic snapshot C");
        noise_close(result.linearized_devices[0].intrinsic_noise.complementary(0, 0),
                    accepted_noise.complementary(0, 0),
                    "accepted intrinsic snapshot P");
        require(input[0].intrinsic_noise.covariance.values ==
                        device.intrinsic_noise.covariance.values &&
                    input[0].source == device.source && initial == Vector{.01} &&
                    offset == Vector{fixed},
                "solve changed caller input snapshots");
        if (dc) {
            require(result.waves.incident[0].imag() == 0. && result.waves.outgoing[0].imag() == 0.,
                    "DC deterministic waves must remain real");
            noise_close(result.waves.outgoing_noise.covariance(0, 0),
                        result.waves.outgoing_noise.complementary(0, 0),
                        "DC C=P");
        }
        noise_calls = 0;
        const auto warm =
            solve_conversion_operating_point_power_waves_with_intrinsic_noise(input,
                                                                              {},
                                                                              {law},
                                                                              {provider},
                                                                              result.waves.incident,
                                                                              tight_options(),
                                                                              true,
                                                                              &additional,
                                                                              offset);
        require(warm.iterations == 0 && noise_calls == 1, "warm acceptance provider count differs");
        check_noise(warm.waves, a_columns, b_columns);
        noise_calls = 0;
        auto acceptable = result.waves.incident;
        acceptable[0] += 1e-5;
        auto loose = tight_options();
        loose.absolute_tolerance = 1e-3;
        const auto retained = solve_conversion_operating_point_with_intrinsic_noise(
            input, {}, {law}, {provider}, acceptable, loose, true, &additional, offset);
        require(retained.iterations == 0 && retained.waves.incident == acceptable &&
                    seen_a == acceptable && seen_b == retained.waves.outgoing && noise_calls == 1,
                "accepted tolerance point was silently polished before noise evaluation");
        const auto expected_retained =
            covariance(drivers(acceptable[0].real(), retained.waves.outgoing[0].real()), 1);
        noise_close(retained.linearized_devices[0].intrinsic_noise.covariance(0, 0),
                    expected_retained.covariance(0, 0),
                    "loose accepted noise snapshot");
        if (kind == 1) {
            auto lower_drive = input;
            lower_drive[0].source[0] = .45;
            noise_calls = 0;
            const auto lower = solve_conversion_operating_point_with_intrinsic_noise(
                lower_drive, {}, {law}, {provider}, {}, tight_options(), true, &additional, offset);
            const double lower_root = scalar_root(1., gamma, .45 + gamma * fixed);
            close(lower.waves.incident[0], lower_root, "second-drive bisection root");
            const auto lower_noise =
                covariance(drivers(lower_root, fixed - std::pow(lower_root, 3)), 1);
            noise_close(lower.linearized_devices[0].intrinsic_noise.covariance(0, 0),
                        lower_noise.covariance(0, 0),
                        "drive-dependent intrinsic density");
            require(noise_calls == 1 && std::abs(lower_noise.covariance(0, 0) -
                                                 accepted_noise.covariance(0, 0)) > 1e-13,
                    "second drive did not change accepted intrinsic noise");
        }
        // Empty providers preserve the legacy function type, deterministic path,
        // static noise, and diagnostics, not just the final noise matrix.
        const auto legacy = solve_conversion_operating_point(
            input, {}, {law}, initial, tight_options(), true, &additional, offset);
        const auto empty = solve_conversion_operating_point_with_intrinsic_noise(
            input, {}, {law}, {}, initial, tight_options(), true, &additional, offset);
        require(legacy.waves.incident == empty.waves.incident &&
                    legacy.waves.outgoing == empty.waves.outgoing &&
                    legacy.output_offset == empty.output_offset &&
                    legacy.iterations == empty.iterations &&
                    legacy.backtracks == empty.backtracks &&
                    legacy.scaled_residual == empty.scaled_residual &&
                    legacy.waves.outgoing_noise.covariance.values ==
                        empty.waves.outgoing_noise.covariance.values &&
                    legacy.waves.outgoing_noise.complementary.values ==
                        empty.waves.outgoing_noise.complementary.values,
                "empty-provider legacy compatibility differs");
    }
}

Complex a_wave(Complex v, Complex i, Complex z) {
    return (v + z * i) / (2. * std::sqrt(z.real()));
}

Complex b_wave(Complex v, Complex i, Complex z) {
    return (v - std::conj(z) * i) / (2. * std::sqrt(z.real()));
}

Complex voltage(Complex a, Complex b, Complex z) {
    return (std::conj(z) * a + z * b) / std::sqrt(z.real());
}

Complex current(Complex a, Complex b, Complex z) {
    return (a - b) / std::sqrt(z.real());
}

constexpr double resistance = 35., conductance = .008, cubic = .015;
const Complex emf{1.2, .9};

ConversionLinearization physical_load(const Vector &a, Complex z) {
    const auto forcing = 2. * std::sqrt(z.real()) * a[0];
    double lower = 0., upper = std::norm(forcing);
    for (int k = 0; k < 160; ++k) {
        const double rho = .5 * (lower + upper);
        if (rho * std::norm(1. + z * (conductance + cubic * rho)) < std::norm(forcing)) {
            lower = rho;
        } else {
            upper = rho;
        }
    }
    const auto v = forcing / (1. + z * (conductance + cubic * .5 * (lower + upper)));
    const auto i = (conductance + cubic * std::norm(v)) * v;
    const double p = conductance + 2. * cubic * std::norm(v), root = std::sqrt(z.real());
    const auto q = cubic * v * v, d = 1. + z * p, e = z * q;
    const double determinant = std::norm(d) - std::norm(e);
    const auto u = 2. * root * std::conj(d) / determinant, w = -2. * root * e / determinant;
    return {FrequencyConversionModel(1.,
                                     {{0, 2}},
                                     {1, {1. - root * (p * u + q * std::conj(w))}},
                                     {1, {-root * (p * w + q * std::conj(u))}},
                                     Vector{z}),
            {b_wave(v, i, z)}};
}

void mixed_reference_physical_noise() {
    const double radius =
        scalar_root(1. + resistance * conductance, resistance * cubic, std::abs(emf));
    const auto phase = emf / std::abs(emf), v = radius * phase;
    const auto i = (conductance + cubic * radius * radius) * v;
    const std::vector<Complex> source_drivers{{1.1e-6, .3e-6}, {-.4e-6, .8e-6}};
    const double scale = 1e-8 * std::sqrt(1. + std::norm(v));
    const std::vector<Complex> norton_drivers{{scale, .3 * scale}, {-.2 * scale, .8 * scale}};
    for (const auto &refs :
         std::vector<Vector>{{50., 50.}, {{30., 12.}, {80., -9.}}, {{80., -15.}, {25., 20.}}}) {
        auto source = empty_device({{0, 2}}, {refs[0]}), load = empty_device({{0, 2}}, {refs[1]});
        source.model = FrequencyConversionModel(
            1.,
            {{0, 2}},
            {1, {(resistance - std::conj(refs[0])) / (resistance + refs[0])}},
            zero(1),
            Vector{refs[0]});
        std::vector<Vector> local_source;
        for (auto driver : source_drivers) {
            local_source.push_back({std::sqrt(refs[0].real()) * driver / (resistance + refs[0])});
        }
        source.intrinsic_noise = covariance(local_source, 1);
        load.intrinsic_noise = covariance({{1e-4}}, 1);
        std::size_t calls = 0;
        const ConversionNonlinearDevice law{1, [&](const Vector &a) {
                                                return physical_load(a, refs[1]);
                                            }};
        const ConversionIntrinsicNoiseProvider provider{
            1, [&](const ConversionAcceptedState &state) {
                ++calls;
                const auto accepted_v = voltage(state.incident[0], state.outgoing[0], refs[1]);
                close(accepted_v, v, "provider physical voltage");
                require(state.fixed_output_offset == Vector{0.},
                        "provider did not receive local zero fixed offset");
                const double p = conductance + 2. * cubic * std::norm(accepted_v);
                const auto q = cubic * accepted_v * accepted_v;
                const auto d = 1. + refs[1] * p, e = refs[1] * q;
                const double determinant = std::norm(d) - std::norm(e);
                const double accepted_scale = 1e-8 * std::sqrt(1. + std::norm(accepted_v));
                std::vector<Vector> local;
                for (auto eta : std::vector<Complex>{{accepted_scale, .3 * accepted_scale},
                                                     {-.2 * accepted_scale, .8 * accepted_scale}}) {
                    const auto f = -refs[1] * eta;
                    const auto dv = (std::conj(d) * f - e * std::conj(f)) / determinant;
                    const auto di = p * dv + q * std::conj(dv) + eta;
                    local.push_back({b_wave(dv, di, refs[1])});
                }
                return snapshot(state, covariance(local, 1));
            }};
        const Vector fixed{std::sqrt(refs[0].real()) * emf / (resistance + refs[0]), 0.};
        const Vector initial{.4 * a_wave(v, -i, refs[0]), .4 * a_wave(v, i, refs[1])};
        const auto solved =
            solve_conversion_operating_point_power_waves_with_intrinsic_noise({source, load},
                                                                              {{0, 0, 1, 0}},
                                                                              {law},
                                                                              {provider},
                                                                              initial,
                                                                              tight_options(),
                                                                              true,
                                                                              nullptr,
                                                                              fixed);
        require(calls == 1, "mixed-reference provider must run once");
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto inward = channel == 0 ? -i : i;
            close(solved.waves.incident[channel],
                  a_wave(v, inward, refs[channel]),
                  "physical incident wave");
            close(solved.waves.outgoing[channel],
                  b_wave(v, inward, refs[channel]),
                  "physical outgoing wave");
            close(voltage(solved.waves.incident[channel],
                          solved.waves.outgoing[channel],
                          refs[channel]),
                  v,
                  "physical voltage continuity");
            close(current(solved.waves.incident[channel],
                          solved.waves.outgoing[channel],
                          refs[channel]),
                  inward,
                  "physical opposite inward current");
        }
        std::vector<Vector> ac, bc, vc, ic;
        for (int group = 0; group < 2; ++group) {
            for (auto driver : group == 0 ? source_drivers : norton_drivers) {
                const auto e = group == 0 ? driver : Complex{};
                const auto eta = group == 1 ? driver : Complex{};
                const auto rotated = std::conj(phase) * (e - resistance * eta);
                const auto dv =
                    phase *
                    Complex{rotated.real() /
                                (1. + resistance * (conductance + 3. * cubic * radius * radius)),
                            rotated.imag() /
                                (1. + resistance * (conductance + cubic * radius * radius))};
                const auto di = (e - dv) / resistance;
                ac.push_back({a_wave(dv, -di, refs[0]), a_wave(dv, di, refs[1])});
                bc.push_back({b_wave(dv, -di, refs[0]), b_wave(dv, di, refs[1])});
                vc.push_back({dv, dv});
                ic.push_back({-di, di});
            }
        }
        check_noise(solved.waves, ac, bc);
        const auto vv = covariance(vc, 2), ii = covariance(ic, 2);
        // Reconstruct physical C/P using all a/b/cross blocks, rather than
        // comparing raw wave values across incompatible reference coordinates.
        for (std::size_t row = 0; row < 2; ++row) {
            for (std::size_t col = 0; col < 2; ++col) {
                for (bool is_current : {false, true}) {
                    const auto x = (is_current ? Complex{1.} : std::conj(refs[row])) /
                                   std::sqrt(refs[row].real());
                    const auto y =
                        (is_current ? Complex{-1.} : refs[row]) / std::sqrt(refs[row].real());
                    const auto u = (is_current ? Complex{1.} : std::conj(refs[col])) /
                                   std::sqrt(refs[col].real());
                    const auto w =
                        (is_current ? Complex{-1.} : refs[col]) / std::sqrt(refs[col].real());
                    const auto &aa = solved.waves.incident_noise, &bb = solved.waves.outgoing_noise,
                               &ab = solved.waves.incident_outgoing_noise;
                    const auto c = x * std::conj(u) * aa.covariance(row, col) +
                                   x * std::conj(w) * ab.covariance(row, col) +
                                   y * std::conj(u) * std::conj(ab.covariance(col, row)) +
                                   y * std::conj(w) * bb.covariance(row, col);
                    const auto p =
                        x * u * aa.complementary(row, col) + x * w * ab.complementary(row, col) +
                        y * u * ab.complementary(col, row) + y * w * bb.complementary(row, col);
                    noise_close(c, (is_current ? ii : vv).covariance(row, col), "physical noise C");
                    noise_close(
                        p, (is_current ? ii : vv).complementary(row, col), "physical noise P");
                }
            }
        }
        noise_close(solved.waves.net_noise_into_device_w_per_hz[0] +
                        solved.waves.net_noise_into_device_w_per_hz[1],
                    0.,
                    "connected physical noise power conservation");
    }
}

void correlated_dc_rf_snapshots() {
    auto driven = empty_device({{2, 0}, {2, 1}, {2, 2}});
    auto d = zero(3);
    d(0, 0) = .2;
    d(1, 1) = .4;
    d(2, 2) = .6;
    driven.model = FrequencyConversionModel(1., driven.model.channels(), d, zero(3));
    driven.source = {.3, {.5, .2}, {-.2, .1}};
    driven.reflection = {.25, .3, .2};
    driven.intrinsic_noise = covariance({{1e-5, 2e-5, 3e-5}}, 3);
    auto unregistered = empty_device({{0, 0}});
    unregistered.intrinsic_noise = covariance({{2e-8}}, 1);
    auto replaced_by_zero = empty_device({{0, 1}});
    replaced_by_zero.intrinsic_noise = covariance({{3e-5}}, 1);
    const Devices devices{driven, unregistered, replaced_by_zero};
    std::size_t calls = 0, zero_calls = 0;
    std::vector<Vector> local_drivers;
    const ConversionIntrinsicNoiseProvider provider{
        0, [&](const ConversionAcceptedState &state) {
            ++calls;
            const double scale = 1e-8 * std::sqrt(1. + std::norm(state.incident[1]));
            local_drivers = {{.7 * scale, Complex{1.1, .4} * scale, Complex{-.2, .6} * scale},
                             {-.2 * scale, Complex{.3, -.8} * scale, Complex{.5, .1} * scale}};
            return snapshot(state, covariance(local_drivers, 3));
        }};
    const ConversionIntrinsicNoiseProvider zero_provider{
        2, [&](const ConversionAcceptedState &state) {
            ++zero_calls;
            return snapshot(state, state.jacobian.zero_noise());
        }};
    const auto result = solve_conversion_operating_point_with_intrinsic_noise(
        devices, {}, {}, {zero_provider, provider}, {}, tight_options(), true);
    require(calls == 1 && zero_calls == 1, "multiple providers were not each evaluated once");
    std::vector<Vector> ac, bc;
    for (const auto &column : local_drivers) {
        Vector a(5), b(5);
        for (std::size_t index = 0; index < 3; ++index) {
            const double gamma = driven.reflection[index].real(), j = d(index, index).real();
            b[index] = column[index] / (1. - gamma * j);
            a[index] = gamma * b[index];
        }
        ac.push_back(a);
        bc.push_back(b);
    }
    ac.push_back(Vector(5));
    bc.push_back({0., 0., 0., 2e-8, 0.});
    check_noise(result.waves, ac, bc);
    require(std::abs(result.waves.outgoing_noise.covariance(0, 1)) > 1e-18 &&
                std::abs(result.waves.outgoing_noise.complementary(1, 2)) > 1e-18,
            "fixture lacks cross-frequency and DC/RF noise correlations");
    noise_close(result.linearized_devices[1].intrinsic_noise.covariance(0, 0),
                4e-16,
                "unregistered static noise was not preserved");
    noise_close(result.linearized_devices[2].intrinsic_noise.covariance(0, 0),
                0.,
                "zero snapshot did not replace static intrinsic noise");
    const auto no_loaded = solve_conversion_operating_point_with_intrinsic_noise(
        devices, {}, {}, {provider, zero_provider}, {}, tight_options(), false);
    require(calls == 2 && zero_calls == 2, "loaded=false omitted a provider");
    for (std::size_t row = 0; row < 5; ++row) {
        for (std::size_t col = 0; col < 5; ++col) {
            noise_close(no_loaded.waves.outgoing_noise.covariance(row, col),
                        result.waves.outgoing_noise.covariance(row, col),
                        "loaded=false outgoing C");
            noise_close(no_loaded.waves.outgoing_noise.complementary(row, col),
                        result.waves.outgoing_noise.complementary(row, col),
                        "loaded=false outgoing P");
        }
    }
    std::size_t failed_calls = 0;
    const ConversionIntrinsicNoiseProvider failing{
        2, [&](const ConversionAcceptedState &) -> ConversionIntrinsicNoiseSnapshot {
            ++failed_calls;
            throw std::logic_error("late snapshot failure");
        }};
    rejects<std::logic_error>([&] {
        solve_conversion_operating_point_with_intrinsic_noise(devices, {}, {}, {provider, failing});
    });
    require(failed_calls == 1 &&
                devices[0].intrinsic_noise.covariance.values ==
                    driven.intrinsic_noise.covariance.values &&
                devices[2].intrinsic_noise.covariance.values ==
                    replaced_by_zero.intrinsic_noise.covariance.values,
            "late provider failure mutated caller-owned noise");
    // Rebuild solely from the returned owning artifacts after provider storage
    // has changed. Snapshot reanalysis cannot depend on retained borrowed views.
    local_drivers.assign(1, Vector(3));
    const auto replay = solve_conversion_operating_point(result.linearized_devices,
                                                         {},
                                                         {},
                                                         {},
                                                         tight_options(),
                                                         true,
                                                         nullptr,
                                                         result.output_offset);
    check_noise(replay.waves, ac, bc);
}

void frozen_simulation_noise_figure() {
    constexpr double kt = 1.380649e-23 * 290.;
    const Channels channels{{0, 1}, {1, 1}, {1, 2}};
    auto device = empty_device(channels);
    device.source[0] = {.8, .3};
    device.source_noise.covariance(0, 0) = 13. * kt;
    device.intrinsic_noise.covariance(1, 1) = 1000. * kt;
    std::size_t laws = 0, providers = 0;
    const auto a = device.source[0];
    const Complex direct = 3. - .4 * std::norm(a), conjugate = -.2 * a * a;
    const double gain = std::norm(direct) + std::norm(conjugate);
    const double q = kt * (2. + std::norm(a));
    ConversionSimulationRequest request;
    request.devices = {device};
    request.operating_point = tight_options();
    request.nonlinear = {{0, [&](const Vector &local) {
                              ++laws;
                              auto d = zero(3), c = zero(3);
                              d(1, 0) = 3. - .4 * std::norm(local[0]);
                              c(1, 0) = -.2 * local[0] * local[0];
                              d(2, 0) = .5;
                              return ConversionLinearization{
                                  FrequencyConversionModel(1., channels, d, c),
                                  {0., (3. - .2 * std::norm(local[0])) * local[0], .5 * local[0]}};
                          }}};
    request.intrinsic_noise_providers = {
        {0, [&](const ConversionAcceptedState &state) {
             ++providers;
             require(state.incident == device.source,
                     "NF provider did not see driven operating point");
             const double accepted_q = kt * (2. + std::norm(state.incident[0]));
             auto noise = state.jacobian.zero_noise();
             noise.covariance(1, 1) = accepted_q;
             noise.complementary(1, 1) = Complex{.2, .1} * accepted_q;
             noise.covariance(2, 2) = .3 * accepted_q;
             return snapshot(state, noise);
         }}};
    request.additional_source_noise = device.model.zero_noise();
    request.additional_source_noise->covariance(0, 0) = 7. * kt;
    request.measurements = {{"out-band", 0, 1, 1.5, 1., {1, 2}, ConversionWaveDirection::outgoing}};
    request.noise_experiments = {{"290K", {{0, 0, 1}}, {{0, 0, 1}}, {0, 1, 1}, 290.},
                                 {"580K", {{0, 0, 1}}, {{0, 0, 1}}, {0, 1, 1}, 580.}};
    const auto result = analyze_conversion_simulation(request);
    require(laws == 1 && providers == 1 && result.operating_point &&
                result.operating_point->iterations == 0,
            "measurement or reference-temperature experiment reran a frozen callback");
    noise_close(result.accepted_devices[0].intrinsic_noise.covariance(1, 1),
                q,
                "simulation accepted noise");
    noise_close(result.waves.outgoing_noise.covariance(1, 1),
                20. * kt * gain + q,
                "ordinary simulation retains source and additional noise");
    noise_close(result.measurements[0].second.noise_power_w,
                .5 * (20. * kt * gain + q + 5. * kt + .3 * q),
                "accepted snapshot measurement");
    for (std::size_t experiment = 0; experiment < 2; ++experiment) {
        const auto &nf = result.noise_experiments[experiment].second;
        const double temperature_factor = experiment == 0 ? 1. : 2.;
        close(nf.reference_gain, gain, "frozen Jacobian NF gain");
        noise_close(nf.reference_output_noise_w_per_hz,
                    temperature_factor * kt * gain,
                    "NF reference noise");
        noise_close(nf.output_noise_w_per_hz,
                    temperature_factor * kt * gain + q,
                    "NF must freeze intrinsic noise and replace source noise");
        close(nf.noise_factor, 1. + q / (temperature_factor * kt * gain), "frozen NF");
    }
    auto invalid = request;
    invalid.operating_point.reset();
    invalid.nonlinear.clear();
    rejects<std::invalid_argument>([&] {
        analyze_conversion_simulation(invalid);
    });
    require(providers == 1, "provider without operating-point request was invoked");
    // Positional aggregate initializers from the previous public request remain valid.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
    const ConversionSimulationRequest old_aggregate{
        {device}, {}, {}, {}, {}, {}, {}, {}, 50., false, {}, {}};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    require(old_aggregate.intrinsic_noise_providers.empty(), "new request member was not appended");
}

void validation_and_failure_counts() {
    auto device = empty_device({{4, 1}, {4, 2}}, {{50., 10.}, {70., -20.}});
    device.source = {{.2, .1}, {-.1, .3}};
    std::size_t laws = 0, providers = 0;
    const ConversionNonlinearDevice law{0, [&](const Vector &) {
                                            ++laws;
                                            return ConversionLinearization{device.model, Vector(2)};
                                        }};
    const ConversionIntrinsicNoiseProvider good{0, [&](const ConversionAcceptedState &state) {
                                                    ++providers;
                                                    return snapshot(state,
                                                                    state.jacobian.zero_noise());
                                                }};
    const auto reject_before = [&](const Devices &input,
                                   const Providers &ps,
                                   const ConversionNoise *extra = nullptr,
                                   const Vector &fixed = {},
                                   const Vector &initial = {}) {
        const auto before_laws = laws, before_providers = providers;
        rejects<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves_with_intrinsic_noise(
                input, {}, {law}, ps, initial, {}, false, extra, fixed);
        });
        require(laws == before_laws && providers == before_providers,
                "invalid setup reached a deterministic or noise callback");
        ++checks;
    };
    reject_before({device}, {good, good});
    reject_before({device}, {{1, good.evaluate}});
    reject_before({device}, {{0, {}}});
    auto invalid = device;
    invalid.intrinsic_noise.covariance(0, 0) = -1e-12;
    reject_before({invalid}, {good});
    invalid = device;
    invalid.source_noise.covariance(0, 0) = -1e-12;
    reject_before({invalid}, {good});
    invalid = device;
    invalid.intrinsic_noise.complementary(0, 0) = 1e-12;
    reject_before({invalid}, {good});
    invalid = device;
    invalid.source_noise.complementary(0, 0) = 1e-12;
    reject_before({invalid}, {good});
    auto extra = device.model.zero_noise();
    extra.complementary(0, 0) = 1e-12;
    reject_before({device}, {good}, &extra);
    reject_before({device}, {good}, nullptr, {0.});
    reject_before({device}, {good}, nullptr, {0., {INFINITY, 0.}});
    reject_before({device}, {good}, nullptr, {}, {0.});
    reject_before({device}, {good}, nullptr, {}, {0., {NAN, 0.}});
    const auto before_legacy = providers;
    rejects<std::invalid_argument>([&] {
        solve_conversion_operating_point_with_intrinsic_noise({device}, {}, {}, {good});
    });
    require(providers == before_legacy, "legacy complex-reference rejection called provider");

    for (int bad = 0; bad < 18; ++bad) {
        const auto before = providers;
        ConversionIntrinsicNoiseProvider invalid_provider{
            0, [&, bad](const ConversionAcceptedState &state) {
                ++providers;
                auto value = snapshot(state, state.jacobian.zero_noise());
                switch (bad) {
                case 0:
                    value.spacing_hz *= 2.;
                    break;
                case 1:
                    value.spacing_hz = NAN;
                    break;
                case 2:
                    std::swap(value.channels[0], value.channels[1]);
                    break;
                case 3:
                    value.channels[1].port = 9;
                    break;
                case 4:
                    value.channels[1].bin = 3;
                    break;
                case 5:
                    value.channels.pop_back();
                    break;
                case 6:
                    value.reference_impedances_ohms[0] =
                        std::conj(value.reference_impedances_ohms[0]);
                    break;
                case 7:
                    value.reference_impedances_ohms[1] += 1.;
                    break;
                case 8:
                    value.reference_impedances_ohms.pop_back();
                    break;
                case 9:
                    value.noise.covariance = zero(1);
                    break;
                case 10:
                    value.noise.complementary = zero(1);
                    break;
                case 11:
                    value.noise.covariance(0, 0) = -1e-12;
                    break;
                case 12:
                    value.noise.covariance(0, 1) = 1e-12;
                    break;
                case 13:
                    value.noise.complementary(1, 0) = 1e-12;
                    break;
                case 14:
                    value.noise.covariance(0, 0) = 1e-12;
                    value.noise.complementary(0, 0) = 2e-12;
                    break;
                case 15:
                    value.noise.covariance(0, 0) = INFINITY;
                    break;
                case 16:
                    value.noise.complementary(0, 0) = Complex{0., NAN};
                    break;
                case 17:
                    value.noise.covariance.values.pop_back();
                    break;
                }
                return value;
            }};
        rejects<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves_with_intrinsic_noise(
                {device}, {}, {law}, {invalid_provider});
        });
        require(providers == before + 1, "invalid snapshot was retried or not evaluated once");
        require(device.intrinsic_noise.covariance.values == Vector(4),
                "invalid snapshot changed caller noise");
        ++checks;
    }
    auto dc = empty_device({{0, 0}});
    for (int bad = 0; bad < 2; ++bad) {
        const ConversionIntrinsicNoiseProvider invalid_dc{
            0, [bad](const ConversionAcceptedState &state) {
                auto value = snapshot(state, state.jacobian.zero_noise());
                value.noise.covariance(0, 0) = 1e-12;
                value.noise.complementary(0, 0) = bad == 0 ? Complex{} : Complex{1e-12, 1e-13};
                return value;
            }};
        rejects<std::invalid_argument>([&] {
            solve_conversion_operating_point_with_intrinsic_noise({dc}, {}, {}, {invalid_dc});
        });
    }
    // Providers are valid for a linear device, with a zero-filled local offset
    // when the caller omitted the global fixed offset.
    std::size_t linear_calls = 0;
    const ConversionIntrinsicNoiseProvider linear{
        0, [&](const ConversionAcceptedState &state) {
            ++linear_calls;
            require(state.incident == device.source && state.outgoing == Vector(2) &&
                        state.fixed_output_offset == Vector(2),
                    "linear accepted view differs");
            auto noise = state.jacobian.zero_noise();
            noise.covariance(1, 1) = 3e-13;
            return snapshot(state, noise);
        }};
    const auto linear_result = solve_conversion_operating_point_power_waves_with_intrinsic_noise(
        {device}, {}, {}, {linear}, {}, {}, true);
    require(linear_calls == 1 && linear_result.iterations == 0, "linear provider count differs");
    noise_close(
        linear_result.waves.outgoing_noise.covariance(1, 1), 3e-13, "linear accepted noise");

    std::size_t thrown_calls = 0;
    const ConversionIntrinsicNoiseProvider throwing{
        0, [&](const ConversionAcceptedState &) -> ConversionIntrinsicNoiseSnapshot {
            ++thrown_calls;
            throw std::logic_error("intentional provider failure");
        }};
    try {
        solve_conversion_operating_point_power_waves_with_intrinsic_noise(
            {device}, {}, {law}, {throwing});
        throw std::runtime_error("provider exception was swallowed");
    } catch (const std::logic_error &error) {
        require(std::string(error.what()) == "intentional provider failure",
                "provider exception changed");
    }
    require(thrown_calls == 1, "throwing provider was retried");

    auto feedback = empty_device({{0, 1}});
    feedback.source[0] = 1.;
    feedback.reflection[0] = 1.;
    std::size_t failure_providers = 0;
    const ConversionIntrinsicNoiseProvider never{0, [&](const ConversionAcceptedState &state) {
                                                     ++failure_providers;
                                                     return snapshot(state,
                                                                     state.jacobian.zero_noise());
                                                 }};
    const ConversionNonlinearDevice cube{
        0, [](const Vector &a) {
            return ConversionLinearization{
                FrequencyConversionModel(
                    1., {{0, 1}}, {1, {-2. * std::norm(a[0])}}, {1, {-a[0] * a[0]}}),
                {-std::norm(a[0]) * a[0]}};
        }};
    auto short_run = tight_options();
    short_run.max_iterations = 1;
    rejects<std::runtime_error>([&] {
        solve_conversion_operating_point_with_intrinsic_noise(
            {feedback}, {}, {cube}, {never}, {.01}, short_run);
    });
    short_run.max_iterations = 50;
    short_run.max_backtracks = 0;
    rejects<std::runtime_error>([&] {
        solve_conversion_operating_point_with_intrinsic_noise(
            {feedback}, {}, {cube}, {never}, {.01}, short_run);
    });
    const ConversionNonlinearDevice singular{
        0, [](const Vector &a) {
            return ConversionLinearization{
                FrequencyConversionModel(1., {{0, 1}}, {1, {1.}}, zero(1)), a};
        }};
    rejects<std::domain_error>([&] {
        solve_conversion_operating_point_with_intrinsic_noise({feedback}, {}, {singular}, {never});
    });
    require(failure_providers == 0, "unconverged or singular solve evaluated intrinsic noise");
}
} // namespace

int main() {
    try {
        cubic_accepted_noise();
        mixed_reference_physical_noise();
        correlated_dc_rf_snapshots();
        frozen_simulation_noise_figure();
        validation_and_failure_counts();
        std::cout << std::setprecision(17) << "{\"passed\":true,\"checks\":" << checks
                  << ",\"maximum_noise_absolute_error\":" << max_noise_error << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
