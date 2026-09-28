#include "rfmodel/multiport_devices.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    constexpr double thermal = 1.380649e-23 * 290.;
    EqualPowerDividerModel divider("splitter", 2);
    const auto s = divider.s_parameters(1e9);
    near(s(1, 0), std::sqrt(0.5));
    near(s(2, 1), 0.);
    // Equal phase inputs combine; opposite phase inputs dissipate in isolation.
    for (double phase_sign : {1., -1.}) {
        LinearNetwork network;
        network.add(s);
        network.terminate(0);
        network.terminate(1, 0., 1.);
        network.terminate(2, 0., phase_sign);
        near(network.solve().outgoing[0], (1. + phase_sign) / std::sqrt(2.));
    }
    const auto noise = passive_thermal_noise(s, 290.).watts_per_hz;
    near(noise(0, 0) / thermal, 0.);
    near(noise(1, 1) / thermal, 0.5);
    near(noise(1, 2) / thermal, -0.5);
    for (std::size_t branches : {2u, 3u, 8u, 64u}) {
        EqualPowerDividerModel lossy("lossy", branches, 3., 75.);
        const auto matrix = lossy.s_parameters(0.);
        double total = 0.;
        for (std::size_t port = 1; port <= branches; ++port) {
            total += std::norm(matrix(port, 0));
        }
        near(total, std::pow(10., -0.3));
        near(lossy.port(branches).reference_impedance, 75.);
        // PSD validation also detects non-passive scattering matrices.
        passive_thermal_noise(matrix, 290.);
    }
    for (double fraction : {0., 0.01, 0.5, 1.}) {
        for (double loss : {0., 3.}) {
            QuadratureCouplerModel coupler("coupler", fraction, loss);
            const auto matrix = coupler.s_parameters(2e9);
            const double gain = std::pow(10., -loss / 10.);
            near(std::norm(matrix(2, 0)), gain * fraction);
            near(matrix(3, 0), 0.);
            const auto covariance = passive_thermal_noise(matrix, 290.).watts_per_hz;
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    Complex product{};
                    for (std::size_t k = 0; k < 4; ++k) {
                        product += matrix(row, k) * std::conj(matrix(column, k));
                    }
                    near(product, row == column ? gain : 0.);
                    near(matrix(row, column), matrix(column, row));
                    near(covariance(row, column) / thermal, row == column ? 1. - gain : 0.);
                }
            }
        }
    }
    // A reflected coupled-port load returns a phase-inverted reflection at input.
    LinearNetwork reflected;
    reflected.add(QuadratureCouplerModel("hybrid", 0.5).s_parameters(1e9));
    reflected.terminate(1);
    reflected.terminate(2, 1.);
    reflected.terminate(3);
    near(reflected.external_s({0})(0, 0), -0.5);
    rejects<std::invalid_argument>([] {
        EqualPowerDividerModel bad("bad", 1);
    });
    rejects<std::invalid_argument>([] {
        EqualPowerDividerModel bad("bad", 65);
    });
    rejects<std::invalid_argument>([] {
        EqualPowerDividerModel bad("bad", 2, -1.);
    });
    rejects<std::invalid_argument>([] {
        QuadratureCouplerModel bad("bad", 1.1);
    });
    rejects<std::invalid_argument>([] {
        QuadratureCouplerModel bad("bad", -0.1);
    });
    rejects<std::invalid_argument>([] {
        QuadratureCouplerModel bad("", 0.5);
    });
    rejects<std::invalid_argument>([] {
        QuadratureCouplerModel bad("bad", 0.5, 0., 0.);
    });
    rejects<std::invalid_argument>([&] {
        divider.s_parameters(-1.);
    });
    rejects<std::invalid_argument>([&] {
        divider.s_parameters(std::numeric_limits<double>::quiet_NaN());
    });
    rejects<std::out_of_range>([&] {
        divider.port(3);
    });
}
