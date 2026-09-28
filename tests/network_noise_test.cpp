#include "rfmodel/network.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    constexpr double kt = 1.380649e-23 * 290.;
    // Two independently noisy resistors at equal temperature: compare the
    // assembled network with thermal noise of its equivalent S matrix.
    IdealRLCModel r1("R1", IdealElement::Resistor, LumpedConnection::SeriesImpedance, 25.);
    IdealRLCModel r2("R2", IdealElement::Resistor, LumpedConnection::SeriesImpedance, 75.);
    LinearNetwork net;
    const auto s1 = r1.s_parameters(1e9), s2 = r2.s_parameters(1e9);
    net.add(s1);
    net.add(s2);
    net.connect(1, 2);
    const auto c1 = passive_thermal_noise(s1, 290.).watts_per_hz;
    const auto c2 = passive_thermal_noise(s2, 290.).watts_per_hz;
    SMatrix combined{4, std::vector<Complex>(16)};
    for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t c = 0; c < 2; ++c) {
            combined(r, c) = c1(r, c);
            combined(r + 2, c + 2) = c2(r, c);
        }
    }
    const auto actual = net.external_noise({0, 3}, {combined}).watts_per_hz;
    const auto expected = passive_thermal_noise(net.external_s({0, 3}), 290.).watts_per_hz;
    for (std::size_t i = 0; i < 4; ++i) {
        near(actual.values[i] / kt, expected.values[i] / kt);
    }
    const auto reverse = net.external_noise({3, 0}, {combined}).watts_per_hz;
    const auto contributions = net.external_noise_contributions({0, 3}, {{c1}, {c2}});
    const auto backwards = net.external_noise_contributions({3, 0}, {{c1}, {c2}});
    for (std::size_t i = 0; i < 4; ++i) {
        near((contributions[0].watts_per_hz.values[i] + contributions[1].watts_per_hz.values[i]) /
                 kt,
             actual.values[i] / kt);
    }
    near(backwards[0].watts_per_hz(0, 1) / kt, contributions[0].watts_per_hz(1, 0) / kt);
    near(reverse(0, 1) / kt, actual(1, 0) / kt);
    // A mismatched but noiseless termination reflects internal noise back.
    LinearNetwork reflected;
    const SMatrix pad{2, {0., 0.5, 0.5, 0.}};
    reflected.add(pad);
    reflected.terminate(1, 0.5);
    near(reflected.external_noise({0}, passive_thermal_noise(pad, 290.)).watts_per_hz(0, 0) / kt,
         0.75 * (1. + 0.25 * 0.25));
    // Cross-port correlation is retained, rather than summing only powers.
    near(reflected.external_noise({0}, {SMatrix{2, {1., 1., 1., 1.}}}).watts_per_hz(0, 0),
         1.25 * 1.25);
    near(reflected.external_noise_contributions({0}, {{{2, {1., 1., 1., 1.}}}})[0].watts_per_hz(0,
                                                                                                0),
         1.25 * 1.25);
    // More external outputs than ports in each independent block.
    LinearNetwork separate;
    separate.add({1, {0.}});
    separate.add({1, {0.}});
    const auto split = separate.external_noise_contributions({1, 0}, {{{1, {2.}}}, {{1, {3.}}}});
    near(split[0].watts_per_hz(1, 1), 2.);
    near(split[0].watts_per_hz(0, 0), 0.);
    near(split[1].watts_per_hz(0, 0), 3.);
    rejects<std::invalid_argument>([&] {
        net.external_noise_contributions({0, 3}, {{c1}});
    });
    rejects<std::invalid_argument>([&] {
        net.external_noise_contributions({0, 3}, {{{1, {0.}}}, {c2}});
    });
    rejects<std::invalid_argument>([&] {
        net.external_noise({0, 3}, {SMatrix{1, {0.}}});
    });
    rejects<std::invalid_argument>([&] {
        net.external_noise({0, 0}, {combined});
    });
    rejects<std::invalid_argument>([&] {
        net.external_noise({0}, {combined});
    });
    LinearNetwork singular;
    singular.add(SMatrix{2, {0., 0., 0., 1.}});
    singular.terminate(1, 1.);
    rejects<std::domain_error>([&] {
        singular.external_noise({0}, {SMatrix{2, {0., 0., 0., 1.}}});
    });
}
