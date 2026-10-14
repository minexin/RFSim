#include <rfmodel/nonlinear/phd_fixed_pump_network.hpp>
#include <cmath>

bool phd_fixed_pump_network_odr_probe() {
    using namespace rfmodel;
    auto network = [] {
        PhdCoefficientData d;
        d.fundamental_hz = 1e9;
        d.port_reference_ohms = {50., 75.};
        d.port_reference_planes = {"pump", "load"};
        d.channels = {{1, 2}, {0, 1}};
        d.pump_channel = 1;
        d.pump_magnitude_sqrt_w = .7;
        d.baseline_outgoing = {.4, .1};
        d.direct = {2, std::vector<Complex>(4)};
        d.conjugate = {2, std::vector<Complex>(4)};
        auto f = PhdCoefficientSlice(d).freeze_pump(.3);
        PhdFixedPumpNetwork n(
            f, {{{0, 1}, f.nominal_incident()[1], 0.}, {{1, 2}, 0., 0.}}, {{{1, 2}, 1.}});
        const auto environment = n.add_environment(
            {FrequencyConversionModel(
                 1e9, {{0, 2}}, SMatrix{1, {.2}}, SMatrix{1, {0.}}, std::vector<Complex>{125.}),
             {{{0, 2}, 0., 0.}},
             {{{0, 2}, {.02, -.01}}},
             {{0, "probe"}}});
        n.connect(0, 1, environment, 0);
        return n;
    }();
    const auto r = network.solve();
    const auto &a = r.channels[0], &b = r.channels[2];
    return r.pump_clamp_exact && r.phd_perturbations[1] == Complex{} &&
           std::abs(std::sqrt(75.) * (a.incident + a.outgoing) -
                    std::sqrt(125.) * (b.incident + b.outgoing)) < 1e-12 &&
           std::abs((a.incident - a.outgoing) / std::sqrt(75.) +
                    (b.incident - b.outgoing) / std::sqrt(125.)) < 1e-12 &&
           b.reference_plane == "probe";
}
