#include "rfmodel/power_wave_network.hpp"
#include "rfmodel/coherent_network.hpp"
#include "rfmodel/term_propagation.hpp"
#include "test_support.hpp"
#include <limits>
using namespace rfmodel;

// From V1-V2=Z*I1, I2=-I1 and Vp+Zref,p*Ip=2*sqrt(Re Zref,p)*ap.
// This supplies physical devices without using the implementation's renormalizer.
static SMatrix series(Complex impedance, Complex a, Complex b) {
    const auto denominator = impedance + a + b;
    const auto transmission = 2. * std::sqrt(a.real()) * std::sqrt(b.real()) / denominator;
    return {2,
            {1. - 2. * a.real() / denominator,
             transmission,
             transmission,
             1. - 2. * b.real() / denominator}};
}

static const std::vector<Complex> refs{{31., 12.}, {73., -21.}, {19., 8.}, {110., 31.}};

static LinearNetwork physical(double frequency) {
    LinearNetwork n(75.); // Construction default is deliberately not the stage reference.
    n.add_power_waves(series(25., refs[0], refs[1]), {refs[0], refs[1]});
    n.add_power_waves(series({75., 2. * std::acos(-1.) * frequency * 2e-9}, refs[2], refs[3]),
                      {refs[2], refs[3]});
    n.connect(1, 2);
    return n;
}

static Complex voltage(Complex a, Complex b, Complex z) {
    return (std::conj(z) * a + z * b) / std::sqrt(z.real());
}

static Complex current(Complex a, Complex b, Complex z) {
    return (a - b) / std::sqrt(z.real());
}

int main() {
    const std::vector<std::size_t> selected{0, 3};
    for (double reference : {25., 50., 75., 120.}) {
        for (double frequency : {0., 1e9, 2e9}) {
            auto n = physical(frequency);
            const auto before = n.external_s(selected);
            auto reduced = reduce_power_wave_network(n, selected, reference);
            near(reduced.reference_impedance_ohms(), reference);
            near(reduced.port_reference_impedance_ohms(0), reference);
            const Complex z{100., 2. * std::acos(-1.) * frequency * 2e-9};
            const auto expected = series(z, reference, reference);
            const auto actual = reduced.external_s({0, 1});
            for (std::size_t i = 0; i < 4; ++i) {
                near(actual.values[i], expected.values[i]);
                near(n.external_s(selected).values[i], before.values[i]);
            }
            const Complex left{.3, .2}, right{.1, -.04};
            const auto i = 2. * std::sqrt(reference) * (left - right) / (z + 2. * reference);
            reduced.terminate(0, {}, left);
            reduced.terminate(1, {}, right);
            const auto response = reduced.solve();
            near(response.outgoing[0], left - std::sqrt(reference) * i);
            near(response.outgoing[1], right + std::sqrt(reference) * i);
            // Excite the original local coordinates with the same physical real
            // source/load. These boundaries are not zero reflection in local waves.
            for (auto p : {std::size_t{0}, std::size_t{3}}) {
                const auto zr = refs[p];
                const auto d = reference + std::conj(zr);
                n.terminate(p,
                            (reference - zr) / d,
                            2. * std::sqrt(zr.real()) * std::sqrt(reference) / d *
                                (p == 0 ? left : right));
            }
            const auto waves = n.solve();
            near(voltage(waves.incident[1], waves.outgoing[1], refs[1]),
                 voltage(waves.incident[2], waves.outgoing[2], refs[2]));
            near(current(waves.incident[1], waves.outgoing[1], refs[1]), -i);
            near(current(waves.incident[2], waves.outgoing[2], refs[2]), i);
            double net = 0.;
            for (auto p : {std::size_t{0}, std::size_t{3}}) {
                net += std::norm(waves.incident[p]) - std::norm(waves.outgoing[p]);
            }
            near(net, 100. * std::norm(i));
        }
    }
    // Per-frequency reduction feeds unchanged spectral/coherence/identity kernels.
    const auto build = [](double f) {
        return reduce_power_wave_network(physical(f), {0, 3}, 50.);
    };
    const auto spectrum =
        transmit_linear_spectrum({1e9, {{0, .2}, {1, {.3, .1}}, {2, .4}}}, {0, 1}, 50., build);
    near(spectrum.amplitudes.at(0), .1);
    for (int bin : {1, 2}) {
        const Complex transfer = 100. / Complex{200., 2. * std::acos(-1.) * bin * 1e9 * 2e-9};
        near(spectrum.amplitudes.at(bin),
             transfer * (bin == 1 ? Complex{.3, .1} : Complex{.4, 0.}));
    }
    const CoherentComponent one{1, SpectrumKind::source, 1., 7, 1.};
    auto negative = one;
    negative.amplitude = -1.;
    auto independent = one;
    independent.coherence_group = 8;
    const auto coherent = transmit_coherent_network(
        1e9, {{0, one}, {0, negative}, {0, independent}}, {1, 0}, 1, 50., build);
    require(coherent.components.size() == 2, "cancelled source identity remains");
    const Complex transfer = 100. / Complex{200., 2. * std::acos(-1.) * 1e9 * 2e-9};
    near(coherent.total_power_w, std::norm(transfer));
    const std::vector<AmplifierMixingTerm> terms{
        {1, 1, {1, 0, 0}, .2}, {3, 1, {-1, 1, 1}, -.2}, {2, 2, {1, 1, 0}, 0.}};
    const auto transmitted = transmit_linear_terms(1e9, terms, {0, 1}, 50., build);
    require(transmitted.size() == 3, "preserve overlapping and zero terms");
    for (std::size_t p = 0; p < 3; ++p) {
        require(transmitted[p].contributors == terms[p].contributors, "preserve contributors");
        near(transmitted[p].amplitude, terms[p].amplitude * transfer);
    }
    // Ordering is explicit, including a nonreciprocal physical stage.
    LinearNetwork asymmetric;
    asymmetric.add_power_waves({2, {{.1, .05}, {.2, .1}, {.7, -.2}, {-.1, .03}}},
                               {refs[0], refs[3]});
    const auto ordered = reduce_power_wave_network(asymmetric, {0, 1}, 60.).external_s({0, 1});
    const auto reversed = reduce_power_wave_network(asymmetric, {1, 0}, 60.).external_s({0, 1});
    near(reversed(0, 1), ordered(1, 0));
    near(reversed(1, 0), ordered(0, 1));
    // Ideal thru has singular Z and must not require a Z-parameter conversion.
    LinearNetwork thru;
    thru.add_power_waves(series(0., refs[0], refs[1]), {refs[0], refs[1]});
    const auto ideal = reduce_power_wave_network(thru, {1, 0}, 90.).external_s({0, 1});
    near(ideal(1, 0), 1.);
    near(ideal(0, 0), 0.);
    for (auto ports : std::vector<std::vector<std::size_t>>{{}, {0}, {0, 0}, {0, 9}, {1, 3}}) {
        rejects<std::exception>([&] {
            reduce_power_wave_network(physical(0.), ports, 50.);
        });
    }
    for (double reference : {0.,
                             -1.,
                             std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        rejects<std::invalid_argument>([&] {
            reduce_power_wave_network(physical(0.), selected, reference);
        });
    }
    auto driven = physical(0.);
    driven.terminate(0, {}, .1);
    rejects<std::invalid_argument>([&] {
        reduce_power_wave_network(driven, {3}, 50.);
    });
    auto terminated = physical(0.);
    terminated.terminate(0, .2);
    const auto one_port = reduce_power_wave_network(terminated, {3}, 50.);
    require(one_port.external_s({0}).ports == 1, "passive internal termination allowed");
    // The legacy direct call still rejects mixed coordinates, even if the caller
    // supplied a nominal reference matching the constructor default.
    rejects<std::exception>([&] {
        transmit_linear_spectrum({1., {{1, .1}}}, selected, 75., physical);
    });
}
