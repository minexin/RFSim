#include "rfmodel/network.hpp"
#include "rfmodel/optimization.hpp"
#include "rfmodel/rlc_model.hpp"
#include <iomanip>
#include <iostream>

int main() {
    try {
        using namespace rfmodel;
        const double frequency = 1e9, omega = 2. * std::acos(-1.) * frequency;
        const std::vector<OptimizationParameter> parameters{
            {1e-10, 3e-8, 2e-9, OptimizationParameterKind::LogReal},
            {1e-13, 1e-10, 1e-11, OptimizationParameterKind::LogReal}};
        const auto objective = [&](const std::vector<double> &x) {
            IdealRLCModel c("C", IdealElement::Capacitor, LumpedConnection::ShuntAdmittance, x[1]);
            IdealRLCModel l("L", IdealElement::Inductor, LumpedConnection::SeriesImpedance, x[0]);
            LinearNetwork network;
            network.add(c.s_parameters(frequency));
            network.add(l.s_parameters(frequency));
            network.connect(1, 2);
            network.terminate(3, (25. - 50.) / (25. + 50.));
            return std::norm(network.external_s({0})(0, 0));
        };
        OptimizationOptions options;
        options.minimum_step = 1e-9;
        options.target = 1e-14;
        const auto result = minimize_bounded(parameters, objective, options);
        if (!result.has_finite_point || result.stop != OptimizationStop::TargetReached) {
            throw std::runtime_error("LC matching target not reached");
        }
        const double l = result.parameters[0], c = result.parameters[1];
        const Complex input = 1. / (1. / Complex{25., omega * l} + Complex{0., omega * c});
        const auto physical_gamma = (input - 50.) / (input + 50.);
        if (std::abs(l / (25. / omega) - 1.) > 1e-6 || std::abs(c / (.02 / omega) - 1.) > 1e-6 ||
            std::abs(std::norm(physical_gamma) - result.value) > 1e-20) {
            throw std::runtime_error("independent matching reference failed");
        }
        std::cout << std::setprecision(17);
        std::cout << "{\n  \"format\": \"rfmodel.optimization-example\",\n  \"version\": 1,\n"
                  << "  \"algorithm\": \"bounded_coordinate_poll\",\n"
                  << "  \"parameter_order\": [\"series_inductance_h\", \"shunt_capacitance_f\"],\n"
                  << "  \"frequency_hz\": " << frequency << ",\n"
                  << "  \"source_reference_ohms\": 50,\n  \"load_ohms\": 25,\n"
                  << "  \"target_reflected_power_fraction\": " << *options.target << ",\n"
                  << "  \"initial_step\": " << options.initial_step << ",\n"
                  << "  \"minimum_step\": " << options.minimum_step << ",\n"
                  << "  \"contraction\": " << options.contraction << ",\n"
                  << "  \"max_evaluations\": " << options.max_evaluations << ",\n"
                  << "  \"max_iterations\": " << options.max_iterations << ",\n"
                  << "  \"parameter_bounds_and_initial\": [";
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            if (i) {
                std::cout << ",";
            }
            std::cout << "[" << parameters[i].lower << "," << parameters[i].upper << ","
                      << parameters[i].initial << "]";
        }
        std::cout << "],\n  \"parameter_scales\": [\"log\",\"log\"],\n"
                  << "  \"stop\": \"" << optimization_stop_name(result.stop) << "\",\n"
                  << "  \"evaluations\": " << result.evaluations << ",\n"
                  << "  \"iterations\": " << result.iterations << ",\n"
                  << "  \"rejected_evaluations\": " << result.rejected_evaluations << ",\n"
                  << "  \"final_step\": " << result.final_step << ",\n"
                  << "  \"parameters\": [" << l << "," << c << "],\n"
                  << "  \"analytic_parameters\": [" << 25. / omega << "," << .02 / omega << "],\n"
                  << "  \"objective\": " << result.value << ",\n"
                  << "  \"physical_input_impedance_ohms\": [" << input.real() << "," << input.imag()
                  << "],\n"
                  << "  \"physical_reflected_power_fraction\": " << std::norm(physical_gamma)
                  << ",\n"
                  << "  \"history\": [\n";
        for (std::size_t i = 0; i < result.history.size(); ++i) {
            const auto &e = result.history[i];
            if (i) {
                std::cout << ",\n";
            }
            std::cout << "    {\"iteration\":" << e.iteration << ",\"parameters\":["
                      << e.parameters[0] << "," << e.parameters[1] << "],\"value\":" << e.value
                      << "}";
        }
        std::cout << "\n  ]\n}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
