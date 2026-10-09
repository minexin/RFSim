#include "rfmodel/butterworth_filter.hpp"
#include "rfmodel/network.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t order : {2u, 3u, 4u, 7u, 16u, 64u}) {
        for (bool open : {false, true}) {
            ButterworthFilterParameters p;
            p.order = order;
            p.input_stopband_open = open;
            ButterworthFilterModel model("low", p);
            for (double ratio : {0., .1, .7, 1., 2., 10.}) {
                const auto s = model.s_parameters(ratio * 1e9);
                const double power = 1. / (1. + std::pow(ratio, 2. * double(order)));
                require(std::abs(std::norm(s(1, 0)) - power) < 2e-12, "Butterworth magnitude");
                Complex pole_response = 1.;
                for (std::size_t k = 0; k < order; ++k) {
                    const Complex pole = std::polar(
                        1., pi * (2. * double(k) + 1. + double(order)) / (2. * double(order)));
                    pole_response /= Complex{0., ratio} - pole;
                }
                require(std::abs(s(1, 0) - pole_response) < 2e-12, "Butterworth pole phase");
                const auto noise = passive_thermal_noise(s, 290.);
                for (auto value : noise.watts_per_hz.values) {
                    require(std::abs(value) < 1e-32, "lossless ladder noise");
                }
            }
            const auto stop = model.s_parameters(1e308);
            near(stop(0, 0), open ? 1. : -1.);
            near(stop(1, 1), ((order % 2 == 1) == open) ? 1. : -1.);
        }
    }
    for (auto kind : {ButterworthResponse::Lowpass,
                      ButterworthResponse::Highpass,
                      ButterworthResponse::Bandpass,
                      ButterworthResponse::Bandstop}) {
        ButterworthFilterParameters p;
        p.response = kind;
        p.lower_passband_hz = 1e9;
        const bool band =
            kind == ButterworthResponse::Bandpass || kind == ButterworthResponse::Bandstop;
        p.upper_passband_hz = band ? 4e9 : 0.;
        p.passband_attenuation_db = 1.;
        ButterworthFilterModel filter("filter", p);
        near(std::norm(filter.s_parameters(1e9)(1, 0)), std::pow(10., -.1));
        if (band) {
            near(std::norm(filter.s_parameters(4e9)(1, 0)), std::pow(10., -.1));
            near(std::norm(filter.s_parameters(2e9)(1, 0)),
                 kind == ButterworthResponse::Bandpass ? 1. : 0.);
        }
        const auto dc = filter.s_parameters(0.);
        near(std::norm(dc(1, 0)),
             kind == ButterworthResponse::Lowpass || kind == ButterworthResponse::Bandstop ? 1.
                                                                                           : 0.);
    }
    // Explicit third-order physical LC network, independently connected.
    const double omega = 2 * pi * 1e9;
    for (double frequency : {0., 1e8, 7e8, 1e9, 2e9, 1e10}) {
        LinearNetwork net;
        net.add(IdealRLCModel(
                    "l1", IdealElement::Inductor, LumpedConnection::SeriesImpedance, 50. / omega)
                    .s_parameters(frequency));
        net.add(
            IdealRLCModel(
                "c", IdealElement::Capacitor, LumpedConnection::ShuntAdmittance, 2. / (50. * omega))
                .s_parameters(frequency));
        net.add(IdealRLCModel(
                    "l2", IdealElement::Inductor, LumpedConnection::SeriesImpedance, 50. / omega)
                    .s_parameters(frequency));
        net.connect(1, 2);
        net.connect(3, 4);
        const auto physical = net.external_s({0, 5});
        const auto synthesized = ButterworthFilterModel("low", {}).s_parameters(frequency);
        for (std::size_t i = 0; i < 4; ++i) {
            near(physical.values[i], synthesized.values[i]);
        }
    }
    for (double edge : {1e-200, 1e200}) {
        ButterworthFilterParameters p;
        p.response = ButterworthResponse::Bandpass;
        p.lower_passband_hz = edge;
        p.upper_passband_hz = std::nextafter(edge, std::numeric_limits<double>::infinity());
        ButterworthFilterModel narrow("narrow", p);
        near(std::norm(narrow.s_parameters(p.lower_passband_hz)(1, 0)), .5);
        near(std::norm(narrow.s_parameters(p.upper_passband_hz)(1, 0)), .5);
    }
    rejects<std::invalid_argument>([] {
        ButterworthFilterParameters p;
        p.order = 1;
        ButterworthFilterModel("bad", p);
    });
    rejects<std::invalid_argument>([] {
        ButterworthFilterParameters p;
        p.passband_attenuation_db = 0.;
        ButterworthFilterModel("bad", p);
    });
    rejects<std::invalid_argument>([] {
        ButterworthFilterModel("filter", {}).s_parameters(-1.);
    });
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
