#include "rfmodel/conversion_operating_point.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>

using namespace rfmodel;
using Vector = std::vector<Complex>;
static std::size_t checks = 0;

static ConversionDevice device(const std::vector<ConversionChannel> &channels,
                               const Vector &references,
                               double spacing = 1.) {
    const auto n = channels.size();
    FrequencyConversionModel model(
        spacing, channels, conversion_detail::zero(n), conversion_detail::zero(n), references);
    return {model, Vector(n), Vector(n), model.zero_noise(), model.zero_noise()};
}

static bool same_values(const Vector &a, const Vector &b) {
    const auto scalar = [](double x, double y) {
        return x == y || (std::isnan(x) && std::isnan(y));
    };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&](Complex x, Complex y) {
               return scalar(x.real(), y.real()) && scalar(x.imag(), y.imag());
           });
}

static bool identical(const ConversionDevice &a, const ConversionDevice &b) {
    if (a.model.spacing_hz() != b.model.spacing_hz() ||
        a.model.reference_impedances_ohms() != b.model.reference_impedances_ohms() ||
        a.model.direct().values != b.model.direct().values ||
        a.model.conjugate().values != b.model.conjugate().values ||
        a.model.channels().size() != b.model.channels().size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.model.channels().size(); ++i) {
        if (a.model.channels()[i].port != b.model.channels()[i].port ||
            a.model.channels()[i].bin != b.model.channels()[i].bin) {
            return false;
        }
    }
    return same_values(a.source, b.source) && same_values(a.reflection, b.reflection) &&
           a.source_noise.covariance.values == b.source_noise.covariance.values &&
           a.source_noise.complementary.values == b.source_noise.complementary.values &&
           a.intrinsic_noise.covariance.values == b.intrinsic_noise.covariance.values &&
           a.intrinsic_noise.complementary.values == b.intrinsic_noise.complementary.values;
}

template <class Exception, class Function> static void failure(Function action) {
    rejects<Exception>(action);
    ++checks;
}

static void callback_contracts() {
    const std::vector<ConversionChannel> channels{{4, 1}, {4, 2}};
    const Vector references{{50., 10.}, {70., -20.}};
    auto original = device(channels, references);
    original.source = {{.2, .1}, {-.1, .3}};
    const auto snapshot = original;
    const Vector initial = original.source, fixed(2);
    std::size_t calls = 0;
    const auto make_callback = [&](std::vector<ConversionChannel> actual_channels,
                                   Vector actual_references,
                                   double spacing,
                                   Vector outgoing) {
        return ConversionNonlinearDevice{
            0, [&, actual_channels, actual_references, spacing, outgoing](const Vector &wave) {
                ++calls;
                require(wave == initial, "callback did not receive original local coordinates");
                const auto n = actual_channels.size();
                return ConversionLinearization{FrequencyConversionModel(spacing,
                                                                        actual_channels,
                                                                        conversion_detail::zero(n),
                                                                        conversion_detail::zero(n),
                                                                        actual_references),
                                               outgoing};
            }};
    };
    const auto good = make_callback(channels, references, 1., Vector(2));
    const auto solved = solve_conversion_operating_point_power_waves(
        {original}, {}, {good}, initial, {}, true, nullptr, fixed);
    require(solved.iterations == 0 && calls == 1, "exact initial callback was not validated once");
    require(solved.linearized_devices[0].model.reference_impedances_ohms() == references,
            "local references were not retained");
    std::vector<ConversionNonlinearDevice> wrong;
    auto changed_references = references;
    changed_references[1] += Complex{0., 1.};
    wrong.push_back(make_callback(channels, changed_references, 1., Vector(2)));
    changed_references = references;
    changed_references[1] += 1.;
    wrong.push_back(make_callback(channels, changed_references, 1., Vector(2)));
    changed_references = references;
    changed_references[0] = std::conj(changed_references[0]);
    wrong.push_back(make_callback(channels, changed_references, 1., Vector(2)));
    wrong.push_back(make_callback(channels, {50., 50.}, 1., Vector(2)));
    auto changed_channels = channels;
    std::swap(changed_channels[0], changed_channels[1]);
    wrong.push_back(make_callback(changed_channels, references, 1., Vector(2)));
    changed_channels = channels;
    changed_channels[1].port = 7;
    wrong.push_back(make_callback(changed_channels, references, 1., Vector(2)));
    changed_channels = channels;
    changed_channels[1].bin = 3;
    wrong.push_back(make_callback(changed_channels, references, 1., Vector(2)));
    wrong.push_back(make_callback(channels, references, 2., Vector(2)));
    wrong.push_back(make_callback(channels, references, 1., Vector(1)));
    wrong.push_back(make_callback({{4, 1}}, {references[0]}, 1., Vector(2)));
    wrong.push_back(make_callback(channels, references, 1., {0., {INFINITY, 0.}}));
    wrong.push_back(make_callback(channels, references, 1., {0., {0., NAN}}));
    for (const auto &callback : wrong) {
        failure<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves(
                {original}, {}, {callback}, initial, {}, false, nullptr, fixed);
        });
        require(identical(original, snapshot) && initial == snapshot.source && fixed == Vector(2),
                "failed callback changed caller inputs");
    }
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {good, good});
    });
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {{1, good.evaluate}});
    });
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {{0, {}}});
    });
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point({original}, {}, {});
    });
    auto common = device(channels, {50., 50.});
    auto unequal = device(channels, {75., 75.});
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point({common, unequal}, {}, {});
    });
    solve_conversion_operating_point_power_waves({common, unequal}, {}, {});
}

static void validation_before_callbacks() {
    const auto left = device({{0, 1}, {0, 2}}, {{50., 10.}, {70., -20.}});
    const auto right = device({{1, 2}, {1, 1}}, {{80., 4.}, {30., 12.}});
    const std::vector<ConversionPortConnection> wires{{0, 0, 1, 1}};
    std::size_t calls = 0;
    const ConversionNonlinearDevice callback{0, [&](const Vector &) {
                                                 ++calls;
                                                 return ConversionLinearization{left.model,
                                                                                Vector(2)};
                                             }};
    const auto reject_devices = [&](const std::vector<ConversionDevice> &devices,
                                    const ConversionNoise *extra = nullptr,
                                    const Vector &fixed = {}) {
        const auto before = calls;
        const auto snapshot = devices;
        const auto fixed_snapshot = fixed;
        const auto extra_snapshot = extra ? *extra : left.model.zero_noise();
        failure<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves(
                devices, wires, {callback}, {}, {}, false, extra, fixed);
        });
        require(calls == before, "invalid boundaries/noise reached nonlinear callback");
        for (std::size_t i = 0; i < devices.size(); ++i) {
            require(identical(devices[i], snapshot[i]), "failed validation changed input device");
        }
        require(same_values(fixed, fixed_snapshot), "failed validation changed fixed offset");
        if (extra) {
            require(
                same_values(extra->covariance.values, extra_snapshot.covariance.values) &&
                    same_values(extra->complementary.values, extra_snapshot.complementary.values),
                "failed validation changed additional noise");
        }
    };
    auto invalid = left;
    invalid.source[1] = .1;
    reject_devices({invalid, right});
    invalid = left;
    invalid.reflection[0] = .1;
    reject_devices({invalid, right});
    invalid = left;
    invalid.source_noise.covariance(1, 1) = 1e-20;
    reject_devices({invalid, right});
    auto extra = ConversionNoise{conversion_detail::zero(4), conversion_detail::zero(4)};
    extra.covariance(1, 1) = 1e-20;
    reject_devices({left, right}, &extra);
    extra.covariance(1, 1) = 0.;
    extra.covariance(0, 0) = -1e-20;
    reject_devices({left, right}, &extra);
    invalid = left;
    invalid.intrinsic_noise.covariance(0, 0) = -1e-20;
    reject_devices({invalid, right});
    invalid = left;
    invalid.intrinsic_noise.covariance(0, 0) = 1e-20;
    invalid.intrinsic_noise.complementary(0, 0) = 2e-20;
    reject_devices({invalid, right});
    invalid = left;
    invalid.intrinsic_noise.covariance(0, 1) = 1e-20;
    reject_devices({invalid, right});
    invalid = left;
    invalid.intrinsic_noise.complementary(1, 0) = 1e-20;
    reject_devices({invalid, right});
    invalid = left;
    invalid.source_noise.covariance = conversion_detail::zero(1);
    reject_devices({invalid, right});
    invalid = left;
    invalid.source.pop_back();
    reject_devices({invalid, right});
    invalid = left;
    invalid.reflection.push_back(0.);
    reject_devices({invalid, right});
    invalid = left;
    invalid.source[0] = {NAN, 0.};
    reject_devices({invalid, right});
    invalid = left;
    invalid.reflection[0] = {0., INFINITY};
    reject_devices({invalid, right});
    reject_devices({left, right}, nullptr, {0.});
    reject_devices({left, right}, nullptr, {0., 0., 0., {INFINITY, 0.}});
    invalid = device({{0, 1}, {0, 2}}, {{50., 10.}, {70., -20.}}, 2.);
    reject_devices({invalid, right});
    for (const auto &bad_wires : std::vector<std::vector<ConversionPortConnection>>{
             {{0, 0, 0, 0}}, {{0, 0, 1, 1}, {0, 0, 1, 1}}, {{0, 7, 1, 1}}}) {
        failure<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves({left, right}, bad_wires, {callback});
        });
    }
    failure<std::out_of_range>([&] {
        solve_conversion_operating_point_power_waves({left, right}, {{0, 0, 2, 1}}, {callback});
    });
    invalid = device({{1, 1}}, {{30., 12.}});
    reject_devices({left, invalid});
    invalid = device({{1, 1}, {1, 3}}, {{30., 12.}, {40., -5.}});
    reject_devices({left, invalid});
    for (const auto &bad_initial : std::vector<Vector>{{0.}, {0., 0., 0., {NAN, 0.}}}) {
        failure<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves({left, right}, wires, {}, bad_initial);
        });
    }
    failure<std::invalid_argument>([] {
        solve_conversion_operating_point_power_waves({}, {}, {});
    });
    for (std::size_t kind = 0; kind < 9; ++kind) {
        ConversionOperatingOptions options;
        switch (kind) {
        case 0:
            options.max_iterations = 0;
            break;
        case 1:
            options.max_iterations = 201;
            break;
        case 2:
            options.max_backtracks = 41;
            break;
        case 3:
            options.relative_tolerance = 0.;
            break;
        case 4:
            options.relative_tolerance = .02;
            break;
        case 5:
            options.relative_tolerance = NAN;
            break;
        case 6:
            options.absolute_tolerance = -1.;
            break;
        case 7:
            options.absolute_tolerance = 0.;
            break;
        default:
            options.absolute_tolerance = INFINITY;
        }
        failure<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves({left}, {}, {}, {}, options);
        });
    }
}

static void dc_contracts() {
    const auto dc = device({{0, 0}, {0, 1}}, {30., {70., 20.}});
    failure<std::invalid_argument>([] {
        device({{0, 0}}, {{30., 1.}});
    });
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({dc}, {}, {}, {{0., 1.}, 0.});
    });
    for (bool reflection : {false, true}) {
        auto invalid = dc;
        (reflection ? invalid.reflection : invalid.source)[0] = {0., 1.};
        failure<std::invalid_argument>([&] {
            solve_conversion_operating_point_power_waves({invalid}, {}, {});
        });
    }
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves(
            {dc}, {}, {}, {}, {}, false, nullptr, {{0., 1.}, 0.});
    });
    auto invalid = dc;
    invalid.source_noise.covariance(0, 0) = 1e-20;
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({invalid}, {}, {});
    });
    const ConversionNonlinearDevice bad{0, [dc](const Vector &) {
                                            return ConversionLinearization{dc.model,
                                                                           {{0., 1.}, 0.}};
                                        }};
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({dc}, {}, {bad});
    });
    invalid.source_noise.complementary(0, 0) = 1e-20;
    const auto valid =
        solve_conversion_operating_point_power_waves({invalid}, {}, {}, {}, {}, true);
    near(valid.waves.incident_noise.covariance(0, 0) / 1e-20, 1.);
    near(valid.waves.incident_noise.complementary(0, 0) / 1e-20, 1.);
}

static void failures_and_accepted_point() {
    auto original = device({{0, 1}}, {{50., 10.}});
    original.source[0] = 1.;
    original.reflection[0] = 1.;
    original.source_noise.covariance(0, 0) = 1e-20;
    const auto snapshot = original;
    const ConversionNonlinearDevice cubic{
        0, [original](const Vector &a) {
            return ConversionLinearization{
                FrequencyConversionModel(1.,
                                         {{0, 1}},
                                         {1, {-2. * std::norm(a[0])}},
                                         {1, {-a[0] * a[0]}},
                                         original.model.reference_impedances_ohms()),
                {-std::norm(a[0]) * a[0]}};
        }};
    const auto solved =
        solve_conversion_operating_point_power_waves({original}, {}, {cubic}, {.01}, {}, true);
    require(solved.iterations > 1 && solved.backtracks > 0, "actual line search was not exercised");
    const double root = solved.waves.incident[0].real();
    require(std::abs(root + root * root * root - 1.) < 2e-9, "cubic physical boundary differs");
    near(solved.waves.outgoing[0], -root * root * root);
    const auto local = cubic.evaluate(solved.waves.incident);
    near(solved.linearized_devices[0].model.direct()(0, 0), local.jacobian.direct()(0, 0));
    near(solved.linearized_devices[0].model.conjugate()(0, 0), local.jacobian.conjugate()(0, 0));
    near(solved.output_offset[0], 2. * root * root * root);
    const double radial = -3. * root * root / (1. + 3. * root * root);
    const double tangent = -root * root / (1. + root * root);
    near(solved.waves.outgoing_noise.covariance(0, 0) / 1e-20,
         .5 * (radial * radial + tangent * tangent));
    near(solved.waves.outgoing_noise.complementary(0, 0) / 1e-20,
         .5 * (radial * radial - tangent * tangent));
    ConversionOperatingOptions options;
    options.max_iterations = 1;
    failure<std::runtime_error>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {cubic}, {.01}, options);
    });
    options.max_iterations = 50;
    options.max_backtracks = 0;
    failure<std::runtime_error>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {cubic}, {.01}, options);
    });
    // Loose tolerance must preserve the accepted nonlinear point, not silently
    // replace it by a further affine solve used to compute noise.
    options = {};
    options.absolute_tolerance = .02;
    const Vector accepted{.68};
    const auto warm = solve_conversion_operating_point_power_waves(
        {original}, {}, {cubic}, accepted, options, true);
    require(warm.iterations == 0 && warm.waves.incident == accepted,
            "noise solve replaced accepted incident waves");
    near(warm.waves.outgoing[0], -.68 * .68 * .68);
    near(warm.linearized_devices[0].model.direct()(0, 0), -2. * .68 * .68);
    near(warm.linearized_devices[0].model.conjugate()(0, 0), -.68 * .68);
    near(warm.output_offset[0], 2. * .68 * .68 * .68);
    const double ar = 1. / (1. + 3. * .68 * .68), at = 1. / (1. + .68 * .68);
    const double br = -3. * .68 * .68 * ar, bt = -.68 * .68 * at;
    near(warm.waves.incident_noise.covariance(0, 0) / 1e-20, .5 * (ar * ar + at * at));
    near(warm.waves.incident_noise.complementary(0, 0) / 1e-20, .5 * (ar * ar - at * at));
    near(warm.waves.outgoing_noise.covariance(0, 0) / 1e-20, .5 * (br * br + bt * bt));
    near(warm.waves.outgoing_noise.complementary(0, 0) / 1e-20, .5 * (br * br - bt * bt));
    near(warm.waves.incident_outgoing_noise.covariance(0, 0) / 1e-20, .5 * (ar * br + at * bt));
    near(warm.waves.incident_outgoing_noise.complementary(0, 0) / 1e-20, .5 * (ar * br - at * bt));
    near(warm.waves.net_noise_into_device_w_per_hz[0] / 1e-20,
         .5 * (ar * ar + at * at - br * br - bt * bt));
    const double error = std::abs(.68 - (1. - .68 * .68 * .68));
    near(warm.scaled_residual, error / (.02 + 1e-9 * (1. - .68 * .68 * .68)));
    const ConversionNonlinearDevice singular{
        0, [original](const Vector &a) {
            return ConversionLinearization{
                FrequencyConversionModel(
                    1., {{0, 1}}, {1, {1.}}, {1, {0.}}, original.model.reference_impedances_ohms()),
                a};
        }};
    failure<std::domain_error>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {singular});
    });
    const ConversionNonlinearDevice overflow{0, [](const Vector &) -> ConversionLinearization {
                                                 throw std::overflow_error("fixture law overflow");
                                             }};
    failure<std::overflow_error>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {overflow});
    });
    require(identical(original, snapshot), "solver changed original device on success/failure");
}

// Independent nondimensionalized V/I equations cover impedances whose unscaled
// sum overflows double, including MSVC where long double has double range.
// This intentionally does not call production connection coefficients.
static Vector connected_incident(const Vector &outgoing, const Vector &references) {
    const double scale = std::max({references[0].real(),
                                   references[1].real(),
                                   std::abs(references[0].imag()),
                                   std::abs(references[1].imag())});
    const auto z0 = references[0] / scale, z1 = references[1] / scale;
    const double r0 = std::sqrt(z0.real()), r1 = std::sqrt(z1.real());
    // I' = I sqrt(scale), V' = V/sqrt(scale); eliminate physical V0'=V1'
    // and I0'=-I1' in b=(V'-conj(z')I')/(2 sqrt(Re z')).
    const auto current = (2. * r1 * outgoing[1] - 2. * r0 * outgoing[0]) / std::conj(z0 + z1);
    const auto voltage = 2. * r0 * outgoing[0] + std::conj(z0) * current;
    return {(voltage + z0 * current) / (2. * r0), (voltage - z1 * current) / (2. * r1)};
}

static void same_device_conversion() {
    // Deliberately interleaved channel order and genuinely nonzero direct and
    // conjugate frequency conversion on a self-wired, two-physical-port device.
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 2}, {0, 2}, {1, 1}};
    const Vector references{{41., 12.}, {72., -9.}, {33., -17.}, {88., 23.}};
    const auto original = device(channels, references);
    const Vector outgoing{{.12, .03}, {-.08, .11}, {.05, -.06}, {-.02, .09}};
    Vector initial(4);
    for (const auto &pair : {std::pair<std::size_t, std::size_t>{0, 3}, {2, 1}}) {
        const auto a = connected_incident({outgoing[pair.first], outgoing[pair.second]},
                                          {references[pair.first], references[pair.second]});
        initial[pair.first] = a[0];
        initial[pair.second] = a[1];
    }
    const ConversionNonlinearDevice law{
        0, [=](const Vector &wave) {
            auto direct = conversion_detail::zero(4), conjugate = direct;
            Vector output(4);
            for (std::size_t i = 0; i < 4; ++i) {
                direct(i, i) = .1;
                direct(i, (i + 1) % 4) = {.03, -.01};
                conjugate(i, (i + 2) % 4) = {-.02, .015};
                for (std::size_t j = 0; j < 4; ++j) {
                    output[i] += direct(i, j) * wave[j] + conjugate(i, j) * std::conj(wave[j]);
                }
            }
            output[2] -= .03 * std::norm(wave[0]) * wave[0];
            direct(2, 0) -= .06 * std::norm(wave[0]);
            conjugate(2, 0) -= .03 * wave[0] * wave[0];
            output[3] += .02 * wave[1] * wave[1];
            direct(3, 1) += .04 * wave[1];
            return ConversionLinearization{
                FrequencyConversionModel(1., channels, direct, conjugate, references), output};
        }};
    const auto local = law.evaluate(initial);
    Vector offset(4);
    for (std::size_t i = 0; i < 4; ++i) {
        offset[i] = outgoing[i] - local.outgoing[i];
    }
    const auto warm = solve_conversion_operating_point_power_waves(
        {original}, {{0, 0, 0, 1}}, {law}, initial, {}, true, nullptr, offset);
    require(warm.iterations == 0, "same-device physical root was rejected");
    const auto cold = solve_conversion_operating_point_power_waves(
        {original}, {{0, 0, 0, 1}}, {law}, {}, {}, true, nullptr, offset);
    require(cold.iterations > 0 && cold.scaled_residual <= 1.,
            "same-device nonlinear conversion did not converge");
    for (std::size_t i = 0; i < 4; ++i) {
        require(std::abs(cold.waves.incident[i] - initial[i]) < 1e-10 &&
                    std::abs(cold.waves.outgoing[i] - outgoing[i]) < 1e-10,
                "same-device nonlinear converted root differs");
    }
}

static void overflow_line_search() {
    auto original = device({{0, 1}}, {{50., 10.}});
    original.source[0] = original.reflection[0] = 1.;
    std::size_t overflows = 0;
    const auto law = [&](const Vector &wave) {
        if (std::abs(wave[0]) > .8) {
            ++overflows;
            throw std::overflow_error("nonlinear fixture range exceeded");
        }
        return ConversionLinearization{FrequencyConversionModel(1.,
                                                                {{0, 1}},
                                                                {1, {-2. * std::norm(wave[0])}},
                                                                {1, {-wave[0] * wave[0]}},
                                                                {{50., 10.}}),
                                       {-std::norm(wave[0]) * wave[0]}};
    };
    const auto result =
        solve_conversion_operating_point_power_waves({original}, {}, {{0, law}}, {.01});
    require(overflows > 0 && result.backtracks > 0 && result.scaled_residual <= 1.,
            "overflowing Newton trial did not backtrack");
    std::size_t invalid_trials = 0;
    const ConversionNonlinearDevice invalid{0, [&](const Vector &wave) {
                                                if (std::abs(wave[0]) > .8) {
                                                    ++invalid_trials;
                                                    throw std::invalid_argument(
                                                        "nonlinear fixture contract changed");
                                                }
                                                return law(wave);
                                            }};
    failure<std::invalid_argument>([&] {
        solve_conversion_operating_point_power_waves({original}, {}, {invalid}, {.01});
    });
    require(invalid_trials == 1, "invalid callback contract was treated as recoverable overflow");
}

static void connection_edge_cases() {
    for (const auto &references : std::vector<Vector>{
             {{50., 20.}, {50., 20.}}, {{20., 0.}, {90., 0.}}, {{1e308, 1e307}, {9e307, -3e307}}}) {
        const auto original = device({{0, 1}, {1, 1}}, references);
        const Vector outgoing{{.1, .2}, {-.3, .4}};
        const auto initial = connected_incident(outgoing, references);
        const auto result = solve_conversion_operating_point_power_waves(
            {original}, {{0, 0, 0, 1}}, {}, initial, {}, true, nullptr, outgoing);
        require(result.iterations == 0, "physical constant-output warm start was rejected");
        require(std::abs(initial[0] - outgoing[1]) > .01,
                "wire fixture accidentally reduced to swapped waves");
        for (std::size_t i = 0; i < 2; ++i) {
            near(result.waves.incident[i], initial[i]);
            near(result.waves.outgoing[i], outgoing[i]);
        }
        const auto cold = solve_conversion_operating_point_power_waves(
            {original}, {{0, 0, 0, 1}}, {}, {}, {}, true, nullptr, outgoing);
        for (std::size_t i = 0; i < 2; ++i) {
            near(cold.waves.incident[i], initial[i]);
        }
    }
    const auto dc = device({{0, 0}, {1, 0}}, {20., 90.});
    const Vector output{.2, -.1};
    const auto initial = connected_incident(output, {20., 90.});
    const auto result = solve_conversion_operating_point_power_waves(
        {dc}, {{0, 0, 0, 1}}, {}, initial, {}, false, nullptr, output);
    require(result.iterations == 0 && result.waves.incident[0].imag() == 0. &&
                result.waves.incident[1].imag() == 0.,
            "unequal real DC physical wire failed");
}

int main() {
    try {
        callback_contracts();
        validation_before_callbacks();
        dc_contracts();
        failures_and_accepted_point();
        connection_edge_cases();
        same_device_conversion();
        overflow_line_search();
        std::cout << "power-wave operating-point contracts passed: " << checks << " rejections\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
