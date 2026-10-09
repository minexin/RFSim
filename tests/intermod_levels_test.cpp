#include "rfmodel/intermod_levels.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>

int run() {
    using namespace rfmodel;
    const auto first =
        polynomial_coefficients_from_intermod_levels(20., {-10., -80., -140.}, {1, -1});
    const auto shifted =
        polynomial_coefficients_from_intermod_levels(20., {0., -60., -110.}, {1, -1});
    for (std::size_t i = 0; i < first.size(); ++i) {
        near(first[i], shifted[i]);
    }
    // The two documented IMN examples imply output IP2=60 and IP3=55 dBm.
    const auto intercepts =
        polynomial_coefficients_from_intercepts(20.,
                                                {{1, -1, 60., 1, InterceptReference::output},
                                                 {2, -1, 55., -1, InterceptReference::output}});
    for (std::size_t i = 0; i < first.size(); ++i) {
        near(first[i], intercepts[i]);
    }

    const std::array<int, 10> first_orders{1, 2, 3, 3, 4, 4, 5, 5, 6, 6};
    const std::array<int, 10> second_orders{-1, -1, -1, -2, -2, -3, -3, -4, -4, -5};
    std::vector<double> levels{-5.};
    std::vector<int> signs;
    for (int order = 2; order <= 11; ++order) {
        levels.push_back(-15. - order);
        signs.push_back(order % 3 ? 1 : -1);
    }
    const double pi = std::acos(-1.);
    for (double reference : {50., 75.}) {
        for (double gain_db : {0., 17.}) {
            const auto a =
                polynomial_coefficients_from_intermod_levels(gain_db, levels, signs, reference);
            require(a.size() == 12 && a[0] == 0., "full IM1..IM11 output");
            // Independently Fourier-project a real waveform one homogeneous
            // order at a time: overlapping higher-order products are not IMn.
            const double input_w = std::pow(10., (levels[0] - gain_db - 30.) / 10.);
            const double peak = std::sqrt(2. * reference * input_w);
            for (int order = 2; order <= 11; ++order) {
                const int k1 = first_orders[order - 2], k2 = second_orders[order - 2];
                const int signed_bin = 31 * k1 + 53 * k2;
                const int bin = std::abs(signed_bin);
                const int samples = 2048;
                std::complex<double> coefficient{};
                for (int i = 0; i < samples; ++i) {
                    const double t = 2. * pi * i / samples;
                    const double voltage = peak * (std::cos(31 * t + .27) + std::cos(53 * t - .19));
                    coefficient += a[order] * std::pow(voltage, order) * std::polar(1., -bin * t);
                }
                const std::complex<double> wave =
                    coefficient * (std::sqrt(2. / reference) / samples);
                const double phase = (k1 * .27 - k2 * .19) * (signed_bin < 0 ? -1. : 1.);
                const double magnitude = std::sqrt(std::pow(10., (levels[order - 1] - 30.) / 10.));
                const std::complex<double> expected =
                    static_cast<double>(signs[order - 2]) * std::polar(magnitude, phase);
                require(std::abs(wave / expected - 1.) < 1e-9,
                        "selected RFAMP_HO reference product reproduces measured output level and "
                        "sign");
            }
        }
    }
    const auto linear = polynomial_coefficients_from_intermod_levels(20., {-10.}, {});
    require(linear.size() == 2, "IM1-only representation is linear");
    near(linear[1], 10.);
    const auto tiny = polynomial_coefficients_from_intermod_levels(20., {0., -1000.}, {1});
    require(tiny[2] > 0. && std::isfinite(tiny[2]), "very weak finite IM is not an omitted order");
    const auto invalid = [&](const std::vector<double> &values, const std::vector<int> &polarity) {
        rejects<std::invalid_argument>([&] {
            polynomial_coefficients_from_intermod_levels(10., values, polarity);
        });
    };
    invalid({}, {});
    invalid(std::vector<double>(12), std::vector<int>(11, 1));
    invalid({0., -40.}, {});
    invalid({0.}, {1});
    invalid({0., -40.}, {0});
    invalid({0., -40.}, {2});
    invalid({std::numeric_limits<double>::quiet_NaN()}, {});
    invalid({0., std::numeric_limits<double>::infinity()}, {1});
    rejects<std::invalid_argument>([] {
        polynomial_coefficients_from_intermod_levels(10., {0.}, {}, -50.);
    });
    rejects<std::overflow_error>([] {
        polynomial_coefficients_from_intermod_levels(0., {1e308, -1e308}, {1});
    });
    rejects<std::overflow_error>([] {
        polynomial_coefficients_from_intermod_levels(0., {0., -1e308}, {1});
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
