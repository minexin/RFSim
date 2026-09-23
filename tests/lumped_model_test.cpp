#include "rfmodel/lumped_model.hpp"
#include "rfmodel/linear_analysis.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    LumpedTwoPortModel resistor("R", LumpedConnection::SeriesImpedance, [](double) {
        return 50.;
    });
    const auto rs = resistor.s_parameters(0);
    near(rs(0, 0), 1. / 3);
    near(rs(1, 0), 2. / 3);
    LumpedTwoPortModel capacitor("C", LumpedConnection::ShuntAdmittance, [](double f) {
        return Complex{0., 2 * 3.14159265358979323846 * f * 1e-12};
    });
    near(capacitor.s_parameters(0)(1, 0), 1.);
    const auto result = analyze_linear(FrequencyGrid{{0., 1e8, 1e9}}, {0, 3}, [&](double f) {
        LinearNetwork net;
        net.add(resistor.s_parameters(f));
        net.add(capacitor.s_parameters(f));
        net.connect(1, 2);
        return net;
    });
    for (std::size_t i = 0; i < result.frequencies_hz.size(); ++i) {
        const Complex y{0., 2 * 3.14159265358979323846 * result.frequencies_hz[i] * 1e-12};
        // Independent ABCD cascade: A=1+R*Y, B=R, C=Y, D=1.
        const auto denominator = 3. + 100. * y;
        near(result.scattering[i](1, 0), 2. / denominator);
        near(result.scattering[i](0, 1), 2. / denominator);
        near(result.scattering[i](0, 0), 1. / denominator);
        near(result.scattering[i](1, 1), (1. - 100. * y) / denominator);
        const auto cs = capacitor.s_parameters(result.frequencies_hz[i]);
        near(std::norm(cs(0, 0)) + std::norm(cs(1, 0)), 1.);
    }
    LumpedTwoPortModel shunt(
        "G",
        LumpedConnection::ShuntAdmittance,
        [](double) {
            return 1. / 75;
        },
        75.);
    near(shunt.s_parameters(1)(0, 0), -1. / 3);
    near(shunt.port(1).reference_impedance, 75.);
    rejects<std::invalid_argument>([&] {
        resistor.s_parameters(-1);
    });
    rejects<std::invalid_argument>([] {
        LumpedTwoPortModel bad("R", LumpedConnection::SeriesImpedance, {});
    });
    rejects<std::out_of_range>([&] {
        resistor.port(2);
    });
    LumpedTwoPortModel singular("active", LumpedConnection::SeriesImpedance, [](double) {
        return -100.;
    });
    rejects<std::domain_error>([&] {
        singular.s_parameters(0);
    });
}
