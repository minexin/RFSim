#include "rfmodel/amplifier_model.hpp"
#include "rfmodel/measurements.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    LinearAmplifierModel matched("matched");
    const auto defaults = matched.s_parameters(1e9);
    near(defaults(0, 0), 0);
    near(defaults(1, 1), 0);
    near(defaults(1, 0), 10);
    near(defaults(0, 1), std::pow(10., -2.5));
    near(matched.port(1).reference_impedance, 50);

    LinearAmplifierParameters parameters;
    parameters.gain_db = 20 * std::log10(2.);
    parameters.reverse_isolation_db = 20;
    parameters.input_impedance_ohms = {100, 0};
    parameters.output_impedance_ohms = {150, 0};
    LinearAmplifierModel bilateral("bilateral", parameters);
    const auto scattering = bilateral.s_parameters(1e9);
    // Solve b2 = 2*a1 + 0.5*a2 with a2=0.2*b2, then source feedback.
    LinearNetwork network;
    network.add(scattering);
    network.terminate(0, 0.25, 1.0);
    network.terminate(1, 0.2);
    const double effective_input_reflection = 1. / 3. + 0.1 * 0.2 * 2. / 0.9;
    const double incident = 1. / (1. - 0.25 * effective_input_reflection);
    const auto waves = network.solve();
    near(waves.incident[0], incident);
    near(waves.outgoing[0], incident * effective_input_reflection);
    near(waves.outgoing[1], 2. * incident / 0.9);
    // Even with a matched load, accepted-power gain differs from |S21|^2.
    const double operating_gain = std::norm(scattering(1, 0)) / (1. - std::norm(scattering(0, 0)));
    near(operating_gain, 4.5);

    parameters.input_impedance_ohms = {50, 50};
    parameters.output_impedance_ohms = {50, -50};
    parameters.gain_phase_degrees = 450;
    parameters.reverse_phase_degrees = -90;
    const auto complex_ports = LinearAmplifierModel("complex", parameters).s_parameters(0);
    near(complex_ports(0, 0), {0.2, 0.4});
    near(complex_ports(1, 1), {0.2, -0.4});
    near(complex_ports(1, 0), {0, 2});
    near(complex_ports(0, 1), {0, -0.1});
    near(input_impedance(complex_ports(0, 0)), parameters.input_impedance_ohms);
    near(reflection_from_impedance(0.), -1.);
    near(reflection_from_impedance(std::numeric_limits<double>::max(),
                                   std::numeric_limits<double>::max()),
         0.);

    parameters.reverse_isolation_db = std::numeric_limits<double>::infinity();
    near(LinearAmplifierModel("unilateral", parameters).s_parameters(1)(0, 1), 0);
    rejects<std::domain_error>([] {
        reflection_from_impedance(-50.);
    });
    rejects<std::invalid_argument>([] {
        reflection_from_impedance(50., 0);
    });
    rejects<std::invalid_argument>([&] {
        matched.s_parameters(-1);
    });
    rejects<std::out_of_range>([&] {
        matched.port(2);
    });
    parameters.reverse_isolation_db = -1;
    rejects<std::invalid_argument>([&] {
        LinearAmplifierModel("bad isolation", parameters);
    });
    parameters.reverse_isolation_db = std::numeric_limits<double>::quiet_NaN();
    rejects<std::invalid_argument>([&] {
        LinearAmplifierModel("nan", parameters);
    });
    parameters.reverse_isolation_db = 50;
    parameters.gain_db = 10000;
    rejects<std::overflow_error>([&] {
        LinearAmplifierModel("overflow", parameters);
    });
}
