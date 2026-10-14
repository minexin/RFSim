#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "../support/test_support.hpp"
#include <iostream>
using namespace rfmodel;

namespace {
// Independent scalar laws for G=20 dB, OP1dB=20 dBm, saturation=23 dBm.
std::pair<double, double> gains(double power) {
    const double r = std::sqrt(power), q = std::pow(10., -.05);
    const double p1 = std::sqrt(std::pow(10., -2.9));
    double gain = 10.;
    if (r > 0. && r <= p1) {
        gain *= 1. - (1. - q) * power / (p1 * p1);
    } else if (r > p1) {
        const double head = std::sqrt(std::pow(10., -.7)) - std::sqrt(.1);
        gain = (std::sqrt(.1) + head * std::tanh(10. * (3. * q - 2.) * (r - p1) / head)) / r;
    }
    const double knee = std::sqrt(std::pow(10., -3.4)), limit = std::sqrt(std::pow(10., -2.8));
    return {gain,
            r <= knee ? 1. : (knee + (limit - knee) * std::tanh((r - knee) / (limit - knee))) / r};
}

std::vector<Complex> fourier(const ModelOperatingPoint &p,
                             const std::set<int> &generating,
                             const std::vector<double> &coefficients,
                             double reference) {
    double power = 0.;
    for (std::size_t i = 0; i < p.channels.size(); ++i) {
        if (p.channels[i].port == 0) {
            power += std::norm(p.incident[i]);
        }
    }
    const auto law = gains(power);
    std::vector<Complex> result(p.channels.size());
    constexpr int samples = 512;
    const double pi = std::acos(-1.);
    for (std::size_t row = 0; row < p.channels.size(); ++row) {
        const auto c = p.channels[row];
        if (c.port != 1 || c.bin == 0) {
            continue;
        }
        for (std::size_t j = 0; j < p.channels.size(); ++j) {
            if (p.channels[j].port == 0 && p.channels[j].bin == c.bin) {
                result[row] += law.first * p.incident[j];
            }
        }
        Complex sum{};
        for (int k = 0; k < samples; ++k) {
            const double t = 2. * pi * k / samples;
            double v = 0.;
            for (std::size_t j = 0; j < p.channels.size(); ++j) {
                if (p.channels[j].port == 0 && generating.count(p.channels[j].bin)) {
                    v += std::sqrt(2. * reference) * law.second *
                         (p.incident[j] * std::polar(1., p.channels[j].bin * t)).real();
                }
            }
            double y = 0.;
            for (std::size_t n = 0; n < coefficients.size(); ++n) {
                y += coefficients[n] * std::pow(v, static_cast<int>(n + 2));
            }
            sum += y * std::polar(1., -c.bin * t);
        }
        result[row] += std::sqrt(2. / reference) * sum / double(samples);
    }
    return result;
}

void physics_and_derivatives() {
    const std::vector<double> coefficients{.13, -.07, 0., .003};
    std::vector<int> bins{3, 1};
    auto adapter = adapt_highorder_amplifier("PA", {20., 20., 23., coefficients, 75.}, bins);
    bins.clear();
    const auto caps = adapter.capabilities();
    require(!caps.linear && !caps.noise && !caps.spectrum && caps.operating_point && caps.coherent,
            "high-order adapter overclaims capabilities");
    require(adapter.descriptor().identity.vendor.empty(), "unexpected vendor claim");
    near(adapter.references().at(1), 75.);
    for (double amplitude : {.005, .06}) {
        ModelOperatingPoint p{1e8,
                              {{0, 3}, {0, 1}, {0, 19}, {1, 0}},
                              {amplitude * Complex{.3, -.4},
                               amplitude * Complex{.6, .2},
                               amplitude * Complex{-.2, .5},
                               0.}};
        for (int b = 1; b <= 19; ++b) {
            p.channels.push_back({1, b});
            p.incident.push_back({.1, .2});
        }
        const auto actual = adapter.linearize(p);
        const auto expected = fourier(p, {1, 3}, coefficients, 75.);
        for (std::size_t i = 0; i < p.channels.size(); ++i) {
            near(actual.jacobian.direct()(3, i), 0.);
            near(actual.jacobian.conjugate()(3, i), 0.);
            near(actual.jacobian.direct()(i, 3), 0.);
            near(actual.jacobian.conjugate()(i, 3), 0.);
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            near(actual.outgoing[i], expected[i]);
        }
        for (std::size_t column = 0; column < p.channels.size(); ++column) {
            if (p.channels[column].bin == 0) {
                continue;
            }
            for (Complex direction : {Complex{.3, -.7}, Complex{-.8, -.2}}) {
                auto plus = p, minus = p;
                constexpr double h = 2e-7;
                plus.incident[column] += h * direction;
                minus.incident[column] -= h * direction;
                const auto hi = fourier(plus, {1, 3}, coefficients, 75.);
                const auto lo = fourier(minus, {1, 3}, coefficients, 75.);
                for (std::size_t row = 0; row < expected.size(); ++row) {
                    const Complex derivative =
                        actual.jacobian.direct()(row, column) * direction +
                        actual.jacobian.conjugate()(row, column) * std::conj(direction);
                    require(std::abs(derivative - (hi[row] - lo[row]) / (2. * h)) < 2e-8,
                            "independent Fourier Jacobian mismatch");
                }
            }
        }
    }
    std::vector<double> high(10);
    high.back() = .2;
    const auto degree11 = adapt_highorder_amplifier("eleven", {20., 20., 23., high}, {1});
    ModelOperatingPoint p{1., {{0, 1}}, {{.03, .02}}};
    for (int b = 1; b <= 11; ++b) {
        p.channels.push_back({1, b});
        p.incident.push_back(0.);
    }
    const auto actual = degree11.linearize(p);
    const auto expected = fourier(p, {1}, high, 50.);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        near(actual.outgoing[i], expected[i]);
    }
}

Complex total(const StreamSnapshot &s, int bin) {
    Complex value{};
    for (const auto &c : s.reduction.components) {
        if (c.bin == bin) {
            value += c.amplitude;
        }
    }
    return value;
}

void graph_and_lifetime() {
    const auto plan = [] {
        CoherentSystem graph(1e8, 50., {{"rf", "clock"}});
        auto input = graph.input("input",
                                 {{1, SpectrumKind::source, 1., 1, {.03, .01}},
                                  {3, SpectrumKind::source, 1., 1, {.01, -.02}}});
        auto adapter = adapt_highorder_amplifier("PA", {20., 20., 23., {.1, -.03}}, {});
        adapter.coherent_stage(graph, "amp", {input}, {"output"});
        return graph.compile();
    }();
    ModelOperatingPoint p{1e8, {{0, 1}, {0, 3}}, {{.03, .01}, {.01, -.02}}};
    for (int b = 1; b <= 9; ++b) {
        p.channels.push_back({1, b});
        p.incident.push_back(0.);
    }
    const auto expected = fourier(p, {1, 3}, {.1, -.03}, 50.);
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto result = plan.execute();
        for (std::size_t i = 2; i < p.channels.size(); ++i) {
            near(total(result.stream("output"), p.channels[i].bin), expected[i]);
        }
        const auto &stats = result.stages().back().statistics;
        near(*stats.input_power_w, .0015);
        near(stats.operating_point->fundamental_amplitude_gain, gains(.0015).first);
        require(stats.polynomial.has_value() && stats.direct_contribution_count == 2,
                "missing high-order stage statistics");
    }
    rejects<CoherentSystemCancelled>([&] {
        plan.execute([] {
            return true;
        });
    });
    require(plan.execute().stream("output").records.size() > 2, "cancelled plan not reusable");
    CoherentSystem groups(1e8, 50., {{"a", ""}, {"b", ""}});
    auto seed = groups.input(
        "seed", {{1, SpectrumKind::source, 1., 1, .04}, {1, SpectrumKind::source, 1., 2, -.04}});
    auto adapter = adapt_highorder_amplifier("PA", {20., 20., 23., {.1}}, {1});
    adapter.coherent_stage(groups, "amp", {seed}, {"out"});
    const auto distinct = groups.compile().execute();
    near(*distinct.stages().back().statistics.input_power_w, .0032);
    std::size_t sources = 0;
    for (const auto &record : distinct.stream("out").records) {
        if (record.component.kind == SpectrumKind::source) {
            ++sources;
            near(std::abs(record.component.amplitude), .04 * gains(.0032).first);
            require(!record.history.empty(), "source history lost");
        }
    }
    require(sources == 2, "unlocked sources merged");
    CoherentSystem mixed(1e8, 50., {{"rf", "clock"}, {"lo", "clock"}});
    auto rf = mixed.input("rf", {{3, SpectrumKind::source, 1., 1, .03}});
    auto translated = mixed.mixer("mixed", rf, {7, 1, 0., 0.});
    auto cutoff = adapt_highorder_amplifier("cutoff", {20., 20., 23., {.1}}, {}, 1);
    cutoff.coherent_stage(mixed, "amp", {translated}, {"out"});
    const auto cut = mixed.compile().execute();
    const auto &stats = cut.stages().back().statistics;
    near(*stats.input_power_w, .0018);
    require(stats.direct_contribution_count == 2 && stats.discarded_direct_contributions == 2,
            "full-history direct cutoff lost");
    near(cut.stream("out").reduction.total_power_w, 0.);
    CoherentSystem cancellation(1e8, 50., {{"rf", "clock"}});
    auto source = cancellation.input("rf", {{1, SpectrumKind::source, 1., 1, .02}});
    auto branches = cancellation.linear_network(
        "split", {{source, 0}}, {0, 1, 2}, {{"plus", 1}, {"minus", 2}}, [](double) {
            LinearNetwork n;
            n.add({3, {0., 0., 0., 1., 0., 0., -1., 0., 0.}});
            return n;
        });
    auto zero = cancellation
                    .linear_network("join",
                                    {{branches[0], 0}, {branches[1], 1}},
                                    {0, 1, 2},
                                    {{"zero", 2}},
                                    [](double) {
                                        LinearNetwork n;
                                        n.add({3, {0., 0., 0., 0., 0., 0., 1., 1., 0.}});
                                        return n;
                                    })
                    .front();
    adapter.coherent_stage(cancellation, "amp", {zero}, {"out"});
    const auto cancelled = cancellation.compile().execute();
    near(*cancelled.stages().back().statistics.input_power_w, 0.);
    near(total(cancelled.stream("out"), 1), 0.);
    require(!cancelled.stream("out").records.empty() &&
                !cancelled.stream("out").records.front().history.empty(),
            "zero origin discarded");
}

void feedback_and_callback_lifetime() {
    const std::vector<ConversionChannel> channels{{0, 1}, {1, 1}, {1, 3}};
    const auto law = [&] {
        auto adapter = adapt_highorder_amplifier("PA", {20., 20., 23., {0., .1}}, {1});
        return adapter.operating_device(0, 1., channels);
    }();
    const auto zero = conversion_detail::zero(3);
    FrequencyConversionModel base(1., channels, zero, zero);
    ConversionDevice active{base, {0., 0., 0.}, {0., 0., 0.}, base.zero_noise(), base.zero_noise()};
    auto feedback = active;
    auto transfer = zero;
    transfer(2, 0) = -.2;
    feedback.model = FrequencyConversionModel(1., {{0, 1}, {0, 3}, {1, 1}}, transfer, zero);
    auto output = [](double a) {
        const auto g = gains(a * a);
        const double limited = a * g.second;
        return a * g.first + 7.5 * limited * limited * limited;
    };
    double lo = 0., hi = .2;
    for (int i = 0; i < 80; ++i) {
        const double mid = (lo + hi) * .5;
        if (mid + .2 * output(mid) > .2) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const double root = (lo + hi) * .5;
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto solved = solve_conversion_operating_point({active, feedback},
                                                             {{0, 1, 1, 0}, {0, 0, 1, 1}},
                                                             {law},
                                                             {},
                                                             {},
                                                             true,
                                                             nullptr,
                                                             {0., 0., 0., 0., 0., .2});
        require(std::abs(solved.waves.incident[0] - root) < 1e-10, "adapter feedback root");
        require(std::abs(solved.waves.outgoing[1] - output(root)) < 1e-10,
                "adapter feedback output");
        const double limited = root * gains(root * root).second;
        require(std::abs(solved.waves.outgoing[2] - 2.5 * std::pow(limited, 3)) < 1e-10,
                "adapter feedback third harmonic");
    }
}

void contracts() {
    const CoherentHighOrderAmplifier model(20., 20., 23., {.1});
    for (auto bins : {std::vector<int>{0},
                      std::vector<int>{-1},
                      std::vector<int>{1, 1},
                      std::vector<int>(513, 1)}) {
        rejects<std::invalid_argument>([&] {
            adapt_highorder_amplifier("PA", model, bins);
        });
    }
    for (std::size_t order : {0U, 257U}) {
        rejects<std::invalid_argument>([&] {
            adapt_highorder_amplifier("PA", model, {}, order);
        });
    }
    rejects<std::invalid_argument>([&] {
        adapt_highorder_amplifier("PA", model, {}, 11, static_cast<AmplifierInputMode>(99));
    });
    rejects<std::invalid_argument>([&] {
        adapt_highorder_amplifier("", model, {});
    });
    const auto adapter = adapt_highorder_amplifier("PA", model, {1});
    const auto zero_drive = adapter.linearize({1., {{0, 1}, {1, 1}, {1, 2}}, {0., 0., 0.}});
    for (std::size_t row = 0; row < 3; ++row) {
        near(zero_drive.outgoing[row], 0.);
        for (std::size_t col = 0; col < 3; ++col) {
            near(zero_drive.jacobian.direct()(row, col), row == 1 && col == 0 ? 10. : 0.);
            near(zero_drive.jacobian.conjugate()(row, col), 0.);
        }
    }
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.s_parameters(1.);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.noise_correlation(1.);
    });
    rejects<ModelCapabilityUnavailable>([&] {
        adapter.transmit({1., {{1, .01}}});
    });
    const ModelOperatingPoint good{1., {{0, 1}, {1, 1}, {1, 2}}, {.01, 0., 0.}};
    for (int failure = 0; failure < 8; ++failure) {
        auto p = good;
        if (failure == 0) {
            p.channels[0].port = 2;
        }
        if (failure == 1) {
            p.channels[2] = p.channels[1];
        }
        if (failure == 2) {
            p.spacing_hz = 0.;
        }
        if (failure == 3) {
            p.incident[0] = std::numeric_limits<double>::infinity();
        }
        if (failure == 4) {
            p.channels[0].bin = 3;
        }
        if (failure == 5) {
            p.channels.pop_back();
            p.incident.pop_back();
        }
        if (failure == 6) {
            p.channels.push_back({1, 0});
            p.incident.push_back(.01);
        }
        if (failure == 7) {
            p.incident.pop_back();
        }
        rejects<std::invalid_argument>([&] {
            adapter.linearize(p);
        });
    }
    const auto direct = adapt_highorder_amplifier("direct", model, {});
    near(direct.linearize({1., {{0, 1}, {1, 1}}, {.02, 0.}}).outgoing[1], .02 * gains(.0004).first);
    for (double reference : {0., -1., std::numeric_limits<double>::infinity()}) {
        rejects<std::invalid_argument>([&] {
            adapt_highorder_amplifier("bad", {20., 20., 23., {.1}, reference}, {1});
        });
    }
    CoherentSystem wrong(1., 75., {{"rf", ""}});
    auto input = wrong.input("in", {{1, SpectrumKind::source, .1, 1, .01}});
    rejects<std::invalid_argument>([&] {
        adapter.coherent_stage(wrong, "bad", {}, {"out"});
    });
    adapter.coherent_stage(wrong, "amp", {input}, {"out"});
    const auto wrong_plan = wrong.compile();
    for (int i = 0; i < 2; ++i) {
        rejects<std::invalid_argument>([&] {
            wrong_plan.execute();
        });
    }
    CoherentSystem graph(1e8, 50., {{"rf", ""}});
    auto source = graph.input("in", {{1, SpectrumKind::source, 1., 1, .01}});
    auto distortion = graph.polynomial("distortion", source, {0., 0., .1}, 2);
    adapter.coherent_stage(graph, "strict", {distortion}, {"out"});
    rejects<std::invalid_argument>([&] {
        graph.compile().execute();
    });
    CoherentSystem cascade(1e8, 50., {{"rf", ""}});
    auto s = cascade.input("in", {{1, SpectrumKind::source, 1., 1, .01}});
    auto d = cascade.polynomial("distortion", s, {0., 0., .1}, 2);
    const auto permissive =
        adapt_highorder_amplifier("PA", model, {}, 11, AmplifierInputMode::propagate_distortion);
    permissive.coherent_stage(cascade, "amp", {d}, {"out"});
    const auto result = cascade.compile().execute();
    const double harmonic = .1 * std::sqrt(25.) * .01 * .01;
    near(total(result.stream("out"), 2), harmonic * gains(harmonic * harmonic).first);
    near(total(result.stream("out"), 4), 0.);
}
} // namespace

int main() {
    try {
        physics_and_derivatives();
        graph_and_lifetime();
        feedback_and_callback_lifetime();
        contracts();
        std::cout << "high-order adapter independent physics and contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
