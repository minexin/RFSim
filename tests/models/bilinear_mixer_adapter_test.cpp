#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "rfmodel/models/ideal_devices.hpp"
#include "../support/test_support.hpp"
#include <iostream>
#include <map>

using namespace rfmodel;

namespace {
// Independent real-signal Fourier convolution. Positive-bin power waves are
// RMS amplitudes, while the DC coefficient is the real wave itself.
std::vector<Complex> product(const ModelOperatingPoint &p, double coefficient) {
    std::map<int, Complex> rf, lo, out;
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        const auto ch = p.channels[i];
        if (ch.port > 1) {
            continue;
        }
        auto &spectrum = ch.port == 0 ? rf : lo;
        spectrum[ch.bin] = p.incident[i] / (ch.bin ? std::sqrt(2.) : 1.);
        if (ch.bin) {
            spectrum[-ch.bin] = std::conj(spectrum[ch.bin]);
        }
    }
    for (const auto &r : rf) {
        for (const auto &l : lo) {
            out[r.first + l.first] += std::sqrt(2.) * coefficient * r.second * l.second;
        }
    }
    std::vector<Complex> result(p.channels.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (p.channels[i].port == 2) {
            result[i] = out[p.channels[i].bin] * (p.channels[i].bin ? std::sqrt(2.) : 1.);
        }
    }
    return result;
}

ConversionDevice empty_device(const std::vector<ConversionChannel> &channels,
                              double reference = 50.) {
    const auto n = channels.size();
    FrequencyConversionModel model(1.,
                                   channels,
                                   conversion_detail::zero(n),
                                   conversion_detail::zero(n),
                                   std::vector<Complex>(n, reference));
    return {model,
            std::vector<Complex>(n),
            std::vector<Complex>(n),
            model.zero_noise(),
            model.zero_noise()};
}

void identity_capabilities_and_ownership() {
    const auto adapter = adapt_bilinear_mixer("three port", 6., 2., 75.);
    const auto &d = adapter.descriptor();
    require(d.identity.type_id == "rfmodel.bilinear_mixer" && d.identity.revision == "1" &&
                d.name == "three port",
            "bilinear identity differs");
    require(d.ports.size() == 3, "bilinear physical port count differs");
    const std::vector<std::string> names{"RF", "LO", "IF"};
    for (std::size_t i = 0; i < 3; ++i) {
        require(d.ports[i].id == i && d.ports[i].name == names[i],
                "bilinear port identity differs");
        near(d.ports[i].reference_impedance, 75.);
    }
    const auto caps = adapter.capabilities();
    require(caps.operating_point && !caps.linear && !caps.noise && !caps.spectrum && !caps.coherent,
            "bilinear adapter claims unsupported capability");
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.s_parameters(1.);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.noise_correlation(1.);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.transmit({1., {{1, 1.}}});
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.spectrum_stage("absent");
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.conversion_device(1., {1}, ModelNoisePolicy::omit_intrinsic_noise);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.conversion_device(1., {1}, ModelNoisePolicy::require_model_noise);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        LinearNetwork n;
        adapter.add_to(n, 1.);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        CoherentSystem system(1., 50., {});
        adapter.coherent_stage(system, "absent", {}, {});
    });
    const auto copy = [] {
        auto original = adapt_bilinear_mixer("owned", 0., 2.);
        auto copied = original;
        return copied;
    }();
    const ModelOperatingPoint point{
        1., {{0, 4}, {1, 3}, {2, 1}, {2, 7}}, {{.3, .2}, {2., -.4}, 0., 0.}};
    const auto callback = [] {
        auto original = adapt_bilinear_mixer("retained", 0., 2.);
        std::vector<ConversionChannel> channels{{0, 4}, {1, 3}, {2, 1}, {2, 7}};
        auto retained = original.operating_device(7, 1., channels);
        channels.clear();
        return retained;
    }();
    require(callback.device == 7, "retained callback lost device index");
    const auto a = copy.linearize(point), b = callback.evaluate(point.incident);
    const auto expected = product(point, .5);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        near(a.outgoing[i], expected[i]);
        near(b.outgoing[i], expected[i]);
    }
    require(a.jacobian.reference_impedances_ohms() == std::vector<Complex>(4, 50.),
            "default mixer reference differs");
}

void independent_values_and_derivatives() {
    // DC, equal-frequency DC products, folded differences, multiple LO tones,
    // collisions, an unused IF bin, arbitrary channel order, and IF incident waves.
    ModelOperatingPoint p{2.5,
                          {{2, 1},
                           {1, 5},
                           {0, 0},
                           {2, 8},
                           {0, 3},
                           {2, 0},
                           {1, 0},
                           {2, 2},
                           {2, 3},
                           {0, 5},
                           {2, 5},
                           {1, 3},
                           {2, 6},
                           {2, 10},
                           {2, 99}},
                          {{.8, -.2},
                           {.4, .7},
                           .2,
                           {.1, .2},
                           {.9, -.3},
                           .4,
                           -.15,
                           {-.4, .2},
                           {.7, -.3},
                           {-.2, .5},
                           {.1, -.4},
                           {1.3, -.6},
                           {.6, .1},
                           {-.5, .3},
                           {.2, .4}}};
    constexpr double gain_db = 4., lo_reference = 1.7;
    const double k = std::pow(10., gain_db / 20.) / lo_reference;
    const auto adapter = adapt_bilinear_mixer("oracle", gain_db, lo_reference, 75.);
    const auto actual = adapter.linearize(p);
    const auto expected = product(p, k);
    require(actual.jacobian.spacing_hz() == p.spacing_hz, "spacing changed");
    require(actual.jacobian.reference_impedances_ohms() ==
                std::vector<Complex>(p.channels.size(), 75.),
            "per-channel reference changed");
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        near(actual.outgoing[i], expected[i]);
        require(actual.jacobian.channels()[i].port == p.channels[i].port &&
                    actual.jacobian.channels()[i].bin == p.channels[i].bin,
                "channels reordered");
        Complex ja{};
        for (std::size_t j = 0; j < p.channels.size(); ++j) {
            ja += actual.jacobian.direct()(i, j) * p.incident[j] +
                  actual.jacobian.conjugate()(i, j) * std::conj(p.incident[j]);
        }
        near(ja, 2. * expected[i]); // F is not the affine offset F-Ja.
    }
    for (std::size_t j = 0; j < p.channels.size(); ++j) {
        for (Complex direction : {Complex{1., 0.}, Complex{0., 1.}}) {
            if (!p.channels[j].bin && direction.imag()) {
                continue;
            }
            // Bilinearity makes a unit central difference exact. Both sides are
            // evaluated by the independent oracle, never by the production law.
            auto plus = p, minus = p;
            plus.incident[j] += direction;
            minus.incident[j] -= direction;
            const auto high = product(plus, k), low = product(minus, k);
            for (std::size_t i = 0; i < p.channels.size(); ++i) {
                const auto derivative = actual.jacobian.direct()(i, j) * direction +
                                        actual.jacobian.conjugate()(i, j) * std::conj(direction);
                near(derivative, .5 * (high[i] - low[i]));
            }
        }
    }
    auto scaled = p, ignored = p;
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        if (p.channels[i].port == 1) {
            scaled.incident[i] *= 3.;
        }
        if (p.channels[i].port == 2) {
            ignored.incident[i] *= -17.;
        }
    }
    const auto tripled = adapter.linearize(scaled), unchanged = adapter.linearize(ignored);
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        near(tripled.outgoing[i], 3. * expected[i]);
        near(unchanged.outgoing[i], expected[i]);
        for (std::size_t j = 0; j < p.channels.size(); ++j) {
            const double factor = p.channels[j].port == 0 ? 3. : 1.;
            near(tripled.jacobian.direct()(i, j), factor * actual.jacobian.direct()(i, j));
            near(tripled.jacobian.conjugate()(i, j), factor * actual.jacobian.conjugate()(i, j));
        }
    }
    p.incident.assign(p.channels.size(), 0.);
    const auto zero = adapter.linearize(p);
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        near(zero.outgoing[i], 0.);
        for (std::size_t j = 0; j < p.channels.size(); ++j) {
            near(zero.jacobian.direct()(i, j), 0.);
            near(zero.jacobian.conjugate()(i, j), 0.);
        }
    }
}

void networks_and_reference_continuity() {
    const std::vector<ConversionChannel> channels{{0, 12}, {1, 10}, {2, 2}, {2, 22}};
    auto mixer = empty_device(channels);
    mixer.source[1] = {2., -.4};
    auto pad = adapt_device_model(MatchedTransmissionModel("pad", 20. * std::log10(2.)),
                                  {"test.pad", "1", "", ""})
                   .conversion_device(1., {12}, ModelNoisePolicy::omit_intrinsic_noise);
    pad.source[0] = {.8, .3};
    const auto solved = solve_conversion_operating_point(
        {mixer, pad},
        {{0, 0, 1, 1}},
        {adapt_bilinear_mixer("driven", 0., 2.).operating_device(0, 1., channels)});
    near(solved.waves.incident[0], .5 * pad.source[0]);
    near(solved.waves.outgoing[2], .25 * pad.source[0] * std::conj(mixer.source[1]));
    near(solved.waves.outgoing[3], .25 * pad.source[0] * mixer.source[1]);
    require(solved.scaled_residual <= 1., "driven mixer failed to converge");

    const std::vector<ConversionChannel> dc{{0, 0}, {1, 0}, {2, 0}};
    auto feedback_mixer = empty_device(dc), splitter = empty_device(dc);
    auto s = conversion_detail::zero(3);
    s(1, 0) = .1;
    s(2, 0) = .2;
    splitter.model = FrequencyConversionModel(1., dc, s, conversion_detail::zero(3));
    const auto feedback = solve_conversion_operating_point(
        {feedback_mixer, splitter},
        {{0, 2, 1, 0}, {0, 0, 1, 1}, {0, 1, 1, 2}},
        {adapt_bilinear_mixer("feedback", -10. * std::log10(2.), 1.).operating_device(0, 1., dc)},
        {},
        {},
        false,
        nullptr,
        {0., 0., 0., 0., 1., 2.});
    const double root = (30. - std::sqrt(500.)) / 2.;
    require(std::abs(feedback.waves.outgoing[2] - root) < 1e-8,
            "analytic DC feedback root differs");
    require(std::abs(feedback.waves.incident[0] - (1. + .1 * root)) < 1e-8, "feedback RF differs");
    require(std::abs(feedback.waves.incident[1] - (2. + .2 * root)) < 1e-8, "feedback LO differs");
    require(feedback.scaled_residual <= 1., "feedback mixer failed to converge");

    for (std::size_t i = 0; i < feedback.waves.incident.size(); ++i) {
        require(feedback.waves.incident[i].imag() == 0. && feedback.waves.outgoing[i].imag() == 0.,
                "feedback DC waves became complex");
    }
    const auto &jacobian = feedback.linearized_devices[0].model;
    near(jacobian.direct()(2, 0) + jacobian.conjugate()(2, 0), feedback.waves.incident[1]);
    near(jacobian.direct()(2, 1) + jacobian.conjugate()(2, 1), feedback.waves.incident[0]);
    near(feedback.output_offset[2], -feedback.waves.outgoing[2]);

    auto seventy_five = empty_device(channels, 75.);
    auto load = empty_device({{0, 2}, {0, 22}}, 50.);
    seventy_five.source[0] = {.4, -.2};
    seventy_five.source[1] = {1.2, .3};
    const auto mixed_refs = solve_conversion_operating_point_power_waves(
        {seventy_five, load},
        {{0, 2, 1, 0}},
        {adapt_bilinear_mixer("75 ohm", 0., 2., 75.).operating_device(0, 1., channels)});
    const auto &a = mixed_refs.waves.incident;
    const auto &b = mixed_refs.waves.outgoing;
    for (std::size_t i = 2; i < 4; ++i) {
        const Complex lo = i == 2 ? std::conj(seventy_five.source[1]) : seventy_five.source[1];
        const Complex f = .5 * seventy_five.source[0] * lo;
        near(b[i], f);
        near(a[i], -.2 * f);
        near(a[i + 2], 2. * std::sqrt(75. * 50.) / 125. * f);
        near(b[i + 2], 0.);
        // Independent physical V/I reconstruction, without renormalization helpers.
        near(std::sqrt(75.) * (a[i] + b[i]), std::sqrt(50.) * (a[i + 2] + b[i + 2]));
        near((a[i] - b[i]) / std::sqrt(75.), -(a[i + 2] - b[i + 2]) / std::sqrt(50.));
    }
}

void invalid_contracts() {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    rejects<std::invalid_argument>([] {
        adapt_bilinear_mixer("", 0., 1.);
    });
    for (double gain : {inf, -inf, nan, 10000., -10000.}) {
        rejects<std::invalid_argument>([&] {
            adapt_bilinear_mixer("bad", gain, 1.);
        });
    }
    for (double amplitude : {0., -1., inf, nan}) {
        rejects<std::invalid_argument>([&] {
            adapt_bilinear_mixer("bad", 0., amplitude);
        });
    }
    for (double reference : {0., -1., inf, nan}) {
        rejects<std::invalid_argument>([&] {
            adapt_bilinear_mixer("bad", 0., 1., reference);
        });
    }
    rejects<std::overflow_error>([&] {
        adapt_bilinear_mixer("overflow", 300., std::numeric_limits<double>::min());
    });
    rejects<std::overflow_error>([&] {
        adapt_bilinear_mixer("underflow", -1000., std::numeric_limits<double>::max());
    });
    const auto adapter = adapt_bilinear_mixer("validation", 0., 1.);
    const ModelOperatingPoint valid{1., {{0, 4}, {1, 3}, {2, 1}, {2, 7}}, {1., 1., 0., 0.}};
    for (double spacing : {0., -1., inf, nan}) {
        auto bad = valid;
        bad.spacing_hz = spacing;
        rejects<std::invalid_argument>([&] {
            adapter.linearize(bad);
        });
    }
    auto bad = valid;
    bad.spacing_hz = std::numeric_limits<double>::max();
    rejects<std::invalid_argument>([&] {
        adapter.linearize(bad);
    });
    bad = valid;
    bad.incident.pop_back();
    rejects<std::invalid_argument>([&] {
        adapter.linearize(bad);
    });
    for (Complex wave : {Complex{inf, 0.}, Complex{0., nan}}) {
        bad = valid;
        bad.incident[0] = wave;
        rejects<std::invalid_argument>([&] {
            adapter.linearize(bad);
        });
    }
    bad = valid;
    bad.channels[0].port = 3;
    rejects<std::invalid_argument>([&] {
        adapter.linearize(bad);
    });
    bad = valid;
    bad.channels[0].bin = -1;
    rejects<std::invalid_argument>([&] {
        adapter.linearize(bad);
    });
    bad = valid;
    bad.channels[3] = bad.channels[2];
    rejects<std::invalid_argument>([&] {
        adapter.linearize(bad);
    });
    rejects<std::invalid_argument>([&] {
        adapter.linearize({1., {}, {}});
    });
    rejects<std::invalid_argument>([&] {
        adapter.linearize({1., {{0, 0}, {1, 0}, {2, 0}}, {Complex{1., .1}, 1., 0.}});
    });
    // A zero operating point still requires both sum and difference output bins.
    for (const auto &channels :
         std::vector<std::vector<ConversionChannel>>{{{0, 4}, {1, 3}, {2, 1}},
                                                     {{0, 4}, {1, 3}, {2, 7}},
                                                     {{0, 4}, {2, 1}, {2, 7}},
                                                     {{1, 3}, {2, 1}, {2, 7}}}) {
        rejects<std::invalid_argument>([&] {
            adapter.linearize({1., channels, std::vector<Complex>(channels.size())});
        });
    }
}
} // namespace

int main() {
    try {
        identity_capabilities_and_ownership();
        independent_values_and_derivatives();
        networks_and_reference_continuity();
        invalid_contracts();
        std::cout << "bilinear mixer adapter passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
