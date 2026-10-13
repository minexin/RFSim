// Standalone two-TU C++17 consumer of the installed native operating-point API.
#include <rfmodel/conversion/conversion_operating_point.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

bool operating_power_wave_odr_probe();

namespace {
using rfmodel::Complex;
using Vector = std::vector<Complex>;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void close(Complex actual,
           Complex expected,
           const std::string &message,
           double absolute_tolerance = 2e-11) {
    const double error = std::abs(actual - expected);
    require(std::isfinite(error) && error <= absolute_tolerance + 2e-9 * std::abs(expected),
            message);
}

void check_mixed_reference_feedback() {
    using namespace rfmodel;
    constexpr double spacing = 1e8;
    const std::vector<ConversionChannel> channels{{0, 2}, {1, 2}};
    const Vector references{{31., 12.}, {73., -21.}};
    const Vector load_references{{19., 8.}};
    const SMatrix zero{2, Vector(4)};
    const FrequencyConversionModel placeholder(spacing, channels, zero, zero, references);
    const FrequencyConversionModel load(spacing, {{0, 2}}, {1, {.2}}, {1, {0.}}, load_references);

    // Specify an exact nonlinear root, then choose its external source. This is
    // an independent scalar equation, not another RFModel network solve.
    const Complex target{.3, -.2}, gamma{.25, .07};
    const Vector offset{{.01, -.02}, {.03, .01}, {.02, -.01}};
    const Complex b0 = -std::norm(target) * target + offset[0];
    const Complex b1 = .6 * target + .1 * std::conj(target) + offset[1];
    const Complex source = target - gamma * b0;
    auto source_noise = placeholder.zero_noise();
    source_noise.covariance(0, 0) = 1e-6;
    const std::vector<ConversionDevice> devices{
        {placeholder, {source, {}}, {gamma, {}}, source_noise, placeholder.zero_noise()},
        {load, {0.}, {0.}, load.zero_noise(), load.zero_noise()}};
    const ConversionNonlinearDevice nonlinear{
        0, [channels, references, spacing = spacing](const Vector &a) {
            SMatrix direct{2, Vector(4)}, conjugate{2, Vector(4)};
            direct(0, 0) = -2. * std::norm(a[0]);
            conjugate(0, 0) = -a[0] * a[0];
            direct(1, 0) = .6;
            conjugate(1, 0) = .1;
            return ConversionLinearization{
                FrequencyConversionModel(spacing, channels, direct, conjugate, references),
                {-std::norm(a[0]) * a[0], .6 * a[0] + .1 * std::conj(a[0])}};
        }};
    ConversionOperatingOptions options;
    options.relative_tolerance = 1e-11;
    options.absolute_tolerance = 1e-13;
    ConversionNoise additional{{3, Vector(9)}, {3, Vector(9)}};
    additional.covariance(0, 0) = 2e-7;
    const auto result = solve_conversion_operating_point_power_waves(
        devices, {{0, 1, 1, 0}}, {nonlinear}, Vector(3), options, true, &additional, offset);

    require(result.iterations > 0 && result.scaled_residual <= 1.,
            "nonlinear solve must iterate and converge");
    require(result.linearized_devices.size() == 2 && result.output_offset.size() == 3,
            "operating-point result shape");
    require(result.linearized_devices[0].model.reference_impedances_ohms() == references &&
                result.linearized_devices[1].model.reference_impedances_ohms() == load_references,
            "accepted Jacobians must retain every original local reference");
    close(result.waves.incident[0], target, "independent nonlinear root");
    close(result.waves.outgoing[0], b0, "true nonlinear outgoing wave");
    close(result.waves.outgoing[1], b1, "wired nonlinear output and fixed offset");

    // Derive both physical-wire rows directly from voltage/current continuity.
    const auto zp = references[1], zq = load_references[0];
    const auto denominator = std::conj(zp + zq);
    const auto self_p = (std::conj(zq) - zp) / denominator;
    const auto self_q = (std::conj(zp) - zq) / denominator;
    const auto coupling = 2. * std::sqrt(zp.real() * zq.real()) / denominator;
    const auto a2 = (coupling * b1 + self_q * offset[2]) / (1. - .2 * self_q);
    const auto b2 = .2 * a2 + offset[2];
    const auto a1 = self_p * b1 + coupling * b2;
    close(result.waves.incident[1], a1, "physical wire includes its self term");
    close(result.waves.incident[2], a2, "physical wire reverse row");
    close(result.waves.outgoing[2], b2, "linear load and its fixed offset");
    const auto vp = (std::conj(zp) * result.waves.incident[1] + zp * result.waves.outgoing[1]) /
                    std::sqrt(zp.real());
    const auto vq = (std::conj(zq) * result.waves.incident[2] + zq * result.waves.outgoing[2]) /
                    std::sqrt(zq.real());
    const auto ip = (result.waves.incident[1] - result.waves.outgoing[1]) / std::sqrt(zp.real());
    const auto iq = (result.waves.incident[2] - result.waves.outgoing[2]) / std::sqrt(zq.real());
    close(vp, vq, "wire voltage continuity");
    close(ip, -iq, "wire current conservation");

    // Loaded noise is evaluated at the same accepted Jacobian. In the external
    // channel its real 2x2 equation is da - gamma*(A*da+B*conj(da)) = e.
    const auto accepted = result.waves.incident[0];
    const auto direct = -2. * std::norm(accepted);
    const auto conjugate = -accepted * accepted;
    close(result.linearized_devices[0].model.direct()(0, 0),
          direct,
          "accepted-point direct Jacobian");
    close(result.linearized_devices[0].model.conjugate()(0, 0),
          conjugate,
          "accepted-point conjugate Jacobian");
    close(result.output_offset[0],
          2. * std::norm(accepted) * accepted + offset[0],
          "accepted-point affine offset includes fixed term");
    close(result.output_offset[1], offset[1], "linear callback offset");
    close(result.output_offset[2], offset[2], "linear device offset");
    const auto p = 1. - gamma * direct, q = -gamma * conjugate;
    const double determinant = std::norm(p) - std::norm(q);
    const auto u = std::conj(p) / determinant, v = -q / determinant;
    constexpr double variance = 1.2e-6;
    require(result.waves.incident_noise.covariance.ports == 3 &&
                result.waves.incident_outgoing_noise.complementary.ports == 3 &&
                result.waves.net_noise_into_device_w_per_hz.size() == 3,
            "complete loaded noise must be available");
    close(result.waves.incident_noise.covariance(0, 0),
          variance * (std::norm(u) + std::norm(v)),
          "independent accepted-point incident covariance",
          2e-18);
    close(result.waves.incident_noise.complementary(0, 0),
          2. * variance * u * v,
          "independent accepted-point incident complementary noise",
          2e-18);

    bool rejected = false;
    try {
        (void)solve_conversion_operating_point(devices, {{0, 1, 1, 0}}, {nonlinear});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "legacy operating-point entry must still reject mixed references");
}
} // namespace

int main() {
    try {
        check_mixed_reference_feedback();
        require(operating_power_wave_odr_probe(), "second translation unit/default-argument probe");
        std::cout << "Installed mixed-reference operating-point consumer passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
