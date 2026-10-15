#include <rfmodel/nonlinear/dc_operating_point.hpp>
#include <rfmodel/linear/frozen_small_signal.hpp>
#include <cmath>
rfmodel::Complex second();

int main() {
    rfmodel::DcDevice resistor{"load", {1}, [](const std::vector<double> &v) {
                                   return rfmodel::DcDeviceEvaluation{
                                       {v[0] / 50}, {.02}, std::nullopt};
                               }};
    auto frozen = rfmodel::freeze_small_signal(rfmodel::solve_dc({1, {resistor}, {.02}}, {0}));
    auto s = frozen.external_s(1000, {1}, {{50, 0}});
    constexpr double kt = 1.380649e-23 * 290.;
    const auto noise = frozen.external_noise(1000, {1}, {{50, 0}}, {{1, {4 * kt * .02}}});
    if (std::abs(noise.watts_per_hz(0, 0).real() / kt - 1.) > 1e-12) {
        return 2;
    }
    return std::abs(s(0, 0)) > 1e-12 || std::abs(second() - rfmodel::Complex(1)) > 1e-12;
}
