#include "rfmodel/noise_figure.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    constexpr double kt=1.380649e-23*290.;
    const SMatrix unity{2,{0.,0.,1.,0.}};
    // Construct F=2+3*|Gamma-(.2+.3i)|^2/(1-|Gamma|^2).
    const Complex optimum{0.2,0.3};
    const NoiseCorrelation c{SMatrix{2,{2.*kt,-3.*std::conj(optimum)*kt,-3.*optimum*kt,1.39*kt}}};
    const auto parameters=extract_noise_parameters(unity,c);
    near(parameters.optimum_source_reflection,optimum);
    near(parameters.minimum_noise_figure_db,10*std::log10(2.));
    near(parameters.noise_resistance_ohms,50.*3.*std::norm(1.+optimum)/4.);
    for (const Complex gamma:{Complex{},Complex{0.4,-0.1},Complex{-0.3,0.7}}) {
        const double reconstructed=std::pow(10.,parameters.minimum_noise_figure_db/10.)+
            4*parameters.noise_resistance_ohms/50.*std::norm(gamma-parameters.optimum_source_reflection)/
            ((1-std::norm(gamma))*std::norm(1.+parameters.optimum_source_reflection));
        near(two_port_noise_figure_db(unity,c,gamma),10*std::log10(reconstructed));
    }
    // Complex gain and input reflection exercise the input-referred transform.
    const SMatrix active{2,{Complex{0.2,-0.1},0.,Complex{2.,1.},0.}};
    const SMatrix inverse{2,{1.,active(0,0),0.,active(1,0)}};
    const auto device_noise=propagate_noise(inverse,c);
    const auto recovered=extract_noise_parameters(active,device_noise,75.);
    near(recovered.optimum_source_reflection,optimum);
    near(recovered.minimum_noise_figure_db,parameters.minimum_noise_figure_db);
    near(recovered.noise_resistance_ohms,1.5*parameters.noise_resistance_ohms);
    const SMatrix pad{2,{0.,0.5,0.5,0.}};
    const auto passive=extract_noise_parameters(pad,passive_thermal_noise(pad,290.));
    near(passive.minimum_noise_figure_db,10*std::log10(4.)); near(passive.optimum_source_reflection,0.);
    const auto noiseless=extract_noise_parameters(unity,{SMatrix{2,{0.,0.,0.,0.}}});
    near(noiseless.noise_resistance_ohms,0.); near(noiseless.minimum_noise_figure_db,0.);
    rejects<std::domain_error>([&]{extract_noise_parameters(unity,{SMatrix{2,{kt,kt,kt,kt}}});});
    rejects<std::invalid_argument>([&]{extract_noise_parameters(unity,c,0.);});
}
