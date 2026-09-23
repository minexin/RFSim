#include "rfmodel/linear_analysis.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    constexpr double kt=1.380649e-23*290.;
    int builds=0, noise_calls=0;
    auto pad=[](double f) { const double t=f/1e9; return SMatrix{2,{0.,t,t,0.}}; };
    const SMatrix second{2,{0.,0.5,0.5,0.}};
    auto build=[&](double f) {
        ++builds;
        LinearNetwork net(75.);
        net.add(pad(f),75.); net.add(second,75.); net.connect(1,2);
        return net;
    };
    const auto result=analyze_linear(FrequencyGrid{{1e8,5e8}}, {3,0}, build,
        [&](double f,const LinearNetwork& net) {
            ++noise_calls; near(net.reference_impedance_ohms(),75.);
            return independent_noise({passive_thermal_noise(pad(f),290.),passive_thermal_noise(second,580.)});
        });
    require(builds==2 && noise_calls==2,"one build and noise callback per frequency");
    require(result.noise_correlation.size()==2,"noise sweep shape");
    for (std::size_t i=0;i<2;++i) {
        const double g=std::pow(result.frequencies_hz[i]/1e9,2);
        const auto& c=result.noise_correlation[i].watts_per_hz;
        // Unequal temperatures make reversing external port order observable.
        near(c(0,0)/kt,0.25*(1-g)+2*0.75);
        near(c(1,1)/kt,(1-g)+g*2*0.75);
        near(c(0,1)/kt,0.);
        near(result.scattering[i](1,0),0.5*std::sqrt(g));
    }
    require(analyze_linear(FrequencyGrid{{1e8}}, {0,3},build).noise_correlation.empty(),"unrequested noise is absent");
    try {
        analyze_linear(FrequencyGrid{{1e8,5e8}}, {0,3},build,
            [&](double f,const LinearNetwork&) {
                if (f==5e8) throw std::invalid_argument("bad noise sample");
                return independent_noise({passive_thermal_noise(pad(f),290.),passive_thermal_noise(second,290.)});
            });
        require(false,"noise callback error was lost");
    } catch (const std::runtime_error& error) {
        const std::string text=error.what();
        require(text.find("500000000")!=std::string::npos && text.find("bad noise sample")!=std::string::npos,"frequency context missing");
    }
    rejects<std::invalid_argument>([]{independent_noise({});});
    rejects<std::invalid_argument>([]{independent_noise({NoiseCorrelation{SMatrix{1,{-1.}}}});});
}
