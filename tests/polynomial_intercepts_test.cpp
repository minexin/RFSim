#include "rfmodel/polynomial_intercepts.hpp"
#include "rfmodel/polynomial_amplifier.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const double pi = std::acos(-1.);
    // Independently project sampled real waveforms instead of reusing the
    // polynomial enumerator or its multiplicity calculation.
    for (double reference : {50., 75.}) {
        for (int order = 2; order <= 11; ++order) {
            const int first = order % 2 ? (order + 1) / 2 : order - 1;
            const int second = first - order;
            const int sign = order % 2 ? -1 : 1;
            const double gain_db = 10.;
            const double ip_dbm = -10.;
            const auto a = polynomial_coefficients_from_intercepts(
                gain_db, {{first, second, ip_dbm, sign}}, reference);
            const auto output_referred = polynomial_coefficients_from_intercepts(
                gain_db,
                {{first, second, ip_dbm + gain_db, sign, InterceptReference::output}},
                reference);
            require(a.size() == static_cast<std::size_t>(order + 1), "highest order output length");
            for (std::size_t i = 0; i < a.size(); ++i) {
                near(a[i], output_referred[i]);
            }
            const double input_w = std::pow(10., (ip_dbm - 30.) / 10.);
            const double peak = std::sqrt(2. * reference * input_w);
            const double phase1 = .27, phase2 = -.19;
            const int signed_bin = 31 * first + 53 * second;
            const int bin = std::abs(signed_bin);
            const int samples = 2048;
            Complex fourier{};
            for (int i = 0; i < samples; ++i) {
                const double t = 2. * pi * i / samples;
                const double x = peak * (std::cos(31 * t + phase1) + std::cos(53 * t + phase2));
                const double y = a[1] * x + a[order] * std::pow(x, order);
                fourier += y * std::polar(1., -bin * t);
            }
            const Complex wave = fourier * (std::sqrt(2. / reference) / samples);
            const double phase = (first * phase1 + second * phase2) * (signed_bin < 0 ? -1 : 1);
            const Complex expected =
                static_cast<double>(sign) * std::polar(std::sqrt(10. * input_w), phase);
            require(std::abs(wave - expected) < 1e-9 * std::abs(expected),
                    "selected homogeneous intermod reaches extrapolated carrier at IP");
        }
    }
    const auto linear = polynomial_coefficients_from_intercepts(20., {});
    require(linear.size() == 2 && linear[0] == 0., "empty specification is linear");
    near(linear[1], 10.);
    const auto legacy = MatchedPolynomialAmplifier::from_intercepts("legacy", 10., 20., 15.);
    const auto common =
        polynomial_coefficients_from_intercepts(10., {{1, 1, 20., 1}, {2, -1, 15., -1}});
    near(common[2], legacy.voltage_coefficient(2));
    near(common[3], legacy.voltage_coefficient(3));
    const auto fourth =
        polynomial_coefficients_from_intercepts(10., {{3, -1, 33., 1, InterceptReference::output}});
    near(fourth[4], .07096267784671509);
    require(fourth[2] == 0. && fourth[3] == 0., "unspecified nonlinear orders are zero");
    const auto balanced = polynomial_coefficients_from_intercepts(0., {{5, -4, -3000., 1}}, 1e300);
    require(std::isfinite(balanced[9]) && balanced[9] > 0.,
            "log calculation handles balanced extreme units");
    const auto equivalent = polynomial_coefficients_from_intercepts(0., {{5, -4, 0., 1}}, 1.);
    require(std::abs(balanced[9] / equivalent[9] - 1.) < 1e-11,
            "balanced units preserve coefficient");
    for (const auto &bad :
         std::vector<TwoToneIntercept>{{0, 1, 20., 1},
                                       {11, 1, 20., 1},
                                       {6, 6, 20., 1},
                                       {std::numeric_limits<int>::min(), 1, 20., 1},
                                       {1, 1, 20., 0},
                                       {1, 1, 20., 2},
                                       {1, 1, std::numeric_limits<double>::quiet_NaN(), 1},
                                       {1, 1, 20., 1, static_cast<InterceptReference>(2)}}) {
        rejects<std::invalid_argument>([&] {
            polynomial_coefficients_from_intercepts(0., {bad});
        });
    }
    rejects<std::invalid_argument>([] {
        polynomial_coefficients_from_intercepts(0., {{1, 1, 20., 1}, {1, -1, 20., 1}});
    });
    rejects<std::invalid_argument>([] {
        polynomial_coefficients_from_intercepts(0., {}, 0.);
    });
    rejects<std::invalid_argument>([] {
        polynomial_coefficients_from_intercepts(std::numeric_limits<double>::infinity(), {});
    });
    rejects<std::invalid_argument>([] {
        polynomial_coefficients_from_intercepts(0., std::vector<TwoToneIntercept>(11));
    });
    for (double extreme : {-1e308, 1e308}) {
        rejects<std::overflow_error>([&] {
            polynomial_coefficients_from_intercepts(extreme, {});
        });
        rejects<std::overflow_error>([&] {
            polynomial_coefficients_from_intercepts(0., {{5, -4, extreme, 1}});
        });
    }
}
