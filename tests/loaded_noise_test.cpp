#include "rfmodel/loaded_noise.hpp"
#include "rfmodel/linear_path_noise.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290.;
    const SMatrix pad{2, {0., 0.5, 0.5, 0.}};
    const auto intrinsic = passive_thermal_noise(pad, 290.);
    const std::vector<Complex> matched{0., 0.};
    const auto equilibrium =
        loaded_noise(pad, intrinsic, matched, thermal_boundary_noise(matched, {290., 290.}));
    near(equilibrium.incident.watts_per_hz(1, 1) / kt, 1.);
    near(equilibrium.outgoing.watts_per_hz(1, 1) / kt, 1.);
    near(equilibrium.net_into_device_w_per_hz[1] / kt, 0.);
    // Noiseless load, mismatched source: compare independently implemented path API.
    const std::vector<Complex> gamma{{0.2, 0.1}, {-0.3, 0.2}};
    const auto result =
        loaded_noise(pad, intrinsic, gamma, thermal_boundary_noise(gamma, {290., 0.}));
    const auto path =
        analyze_linear_path_noise({{"pad", pad}}, {intrinsic}, 290., gamma[0], gamma[1]);
    near(-result.net_into_device_w_per_hz[1] / kt, path.total_output_w_per_hz / kt);
    near(result.incident.watts_per_hz(1, 1) / kt,
         std::norm(gamma[1]) * result.outgoing.watts_per_hz(1, 1) / kt);
    // One-port scalar formula includes direct boundary emission and its reflected copy.
    const Complex s{0.2, 0.3}, g{-0.1, 0.2};
    const double c = 2., e = 3.;
    const auto single = loaded_noise({1, {s}}, {{1, {c}}}, {g}, {{1, {e}}});
    const double denominator = std::norm(1. - s * g);
    near(single.outgoing.watts_per_hz(0, 0), (c + std::norm(s) * e) / denominator);
    near(single.incident.watts_per_hz(0, 0), (std::norm(g) * c + e) / denominator);
    // Lossless thru exchanges thermal power; closed reflection feedback must conserve energy.
    const SMatrix thru{2, {0., 1., 1., 0.}};
    const NoiseCorrelation zero{{2, {0., 0., 0., 0.}}};
    const auto exchange =
        loaded_noise(thru, zero, gamma, thermal_boundary_noise(gamma, {100., 400.}));
    near((exchange.net_into_device_w_per_hz[0] + exchange.net_into_device_w_per_hz[1]) / kt, 0.);
    // Correlated boundary emissions retain complex off-diagonal terms and port ordering.
    const NoiseCorrelation correlated{{2, {2., Complex{0., 1.}, Complex{0., -1.}, 2.}}};
    const auto cross = loaded_noise(thru, zero, matched, correlated);
    near(cross.outgoing.watts_per_hz(0, 1), Complex{0., -1.});
    near(cross.incident.watts_per_hz(0, 1), Complex{0., 1.});
    const auto mirror = loaded_noise({1, {0.}}, {{1, {1.}}}, {1.}, {{1, {0.}}});
    near(mirror.net_into_device_w_per_hz[0], 0.);
    rejects<std::domain_error>([&] {
        loaded_noise(thru, zero, {1., 1.}, zero);
    });
    rejects<std::invalid_argument>([&] {
        loaded_noise(pad, intrinsic, {1.1, 0.}, zero);
    });
    rejects<std::invalid_argument>([&] {
        loaded_noise(pad, intrinsic, {0.}, zero);
    });
    rejects<std::invalid_argument>([&] {
        loaded_noise(pad, intrinsic, matched, {{2, {-1., 0., 0., 1.}}});
    });
    rejects<std::invalid_argument>([] {
        thermal_boundary_noise({0.}, {-1.});
    });
}
