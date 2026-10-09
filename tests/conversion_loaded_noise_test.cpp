#include "rfmodel/conversion_network.hpp"
#include "rfmodel/loaded_noise.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    const Complex s{.5, .2}, gamma{.3, -.1}, ps{.2, .1}, pc{.1, -.05};
    constexpr double ns = 1., nc = .7;
    FrequencyConversionModel model(1e9, {{0, 1}}, {1, {s}}, {1, {0.}});
    const ConversionNoise source{{1, {ns}}, {1, {ps}}};
    const ConversionNoise intrinsic{{1, {nc}}, {1, {pc}}};
    const auto result = model.analyze({0.}, {gamma}, source, intrinsic, {}, true);
    const auto denominator = 1. - s * gamma;
    const auto scale = std::norm(denominator);
    near(result.incident_noise.covariance(0, 0), (ns + std::norm(gamma) * nc) / scale);
    near(result.outgoing_noise.covariance(0, 0), (std::norm(s) * ns + nc) / scale);
    near(result.incident_outgoing_noise.covariance(0, 0), (std::conj(s) * ns + gamma * nc) / scale);
    near(result.incident_noise.complementary(0, 0),
         (ps + gamma * gamma * pc) / (denominator * denominator));
    near(result.outgoing_noise.complementary(0, 0),
         (s * s * ps + pc) / (denominator * denominator));
    near(result.incident_outgoing_noise.complementary(0, 0),
         (s * ps + gamma * pc) / (denominator * denominator));
    near(result.net_noise_into_device_w_per_hz[0],
         ((1. - std::norm(s)) * ns - (1. - std::norm(gamma)) * nc) / scale);
    if (!model.analyze({0.}, {gamma}, source, intrinsic).net_noise_into_device_w_per_hz.empty()) {
        throw std::runtime_error("unrequested loaded statistics were produced");
    }
    constexpr double kt = 1.380649e-23 * 290.;
    const ConversionNoise warm_source{{1, {kt * (1. - std::norm(gamma))}}, {1, {0.}}};
    const ConversionNoise warm_device{{1, {kt * (1. - std::norm(s))}}, {1, {0.}}};
    const auto equilibrium = model.analyze({0.}, {gamma}, warm_source, warm_device, {}, true);
    near(equilibrium.net_noise_into_device_w_per_hz[0] / kt, 0.);
    // A real DC channel has C=P, including the incident/outgoing cross pair.
    FrequencyConversionModel dc(1., {{0, 0}}, {1, {.5}}, {1, {0.}});
    const auto dc_result =
        dc.analyze({0.}, {0.}, {{1, {2.}}, {1, {2.}}}, dc.zero_noise(), {}, true);
    near(dc_result.incident_noise.covariance(0, 0), 2.);
    near(dc_result.incident_noise.complementary(0, 0), 2.);
    near(dc_result.incident_outgoing_noise.covariance(0, 0), 1.);
    near(dc_result.incident_outgoing_noise.complementary(0, 0), 1.);
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
