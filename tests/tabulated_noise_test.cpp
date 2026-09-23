#include "rfmodel/tabulated_noise_model.hpp"
#include "rfmodel/linear_analysis.hpp"
#include "rfmodel/noise_figure.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    constexpr double kt=1.380649e-23*290.;
    TouchstoneData data;
    data.ports=2; data.frequencies_hz={0.,4e9};
    data.matrices={SMatrix{2,{0.,0.,1.,0.}},SMatrix{2,{0.,0.,1.,0.}}};
    NoiseTable table{{1e9,3e9},{NoiseCorrelation{SMatrix{2,{kt,Complex{0.,-kt},Complex{0.,kt},kt}}},
                                NoiseCorrelation{SMatrix{2,{kt,Complex{0.,kt},Complex{0.,-kt},kt}}}}};
    TabulatedNoiseModel model("amplifier",data,table);
    table.samples[0].watts_per_hz(1,1)=99.;
    data.matrices[0](1,0)=99.;
    const NoiseCorrelationProvider& provider=model;
    const auto mid=provider.noise_correlation(2e9).watts_per_hz;
    near(mid(0,0)/kt,1.); near(mid(1,1)/kt,1.); near(mid(0,1)/kt,0.);
    near(model.s_parameters(2e9)(1,0),1.);
    const auto result=analyze_linear(FrequencyGrid{{1e9,2e9,3e9}},{0,1},[&](double f) {
        LinearNetwork net; net.add(model.s_parameters(f)); return net;
    },[&](double f,const LinearNetwork&) { return provider.noise_correlation(f); });
    for (std::size_t i=0;i<3;++i)
        near(two_port_noise_figure_db(result.scattering[i],result.noise_correlation[i]),10*std::log10(2.));
    near(result.noise_correlation[0].watts_per_hz(0,1)/kt,Complex{0.,-1.});
    near(result.noise_correlation[2].watts_per_hz(0,1)/kt,Complex{0.,1.});
    rejects<std::out_of_range>([&]{model.noise_correlation(0.);});
    near(model.s_parameters(0.)(1,0),1.); // Independent S/noise domains.
    NoiseTable single{{2e9},{NoiseCorrelation{mid}}};
    TabulatedNoiseModel clamp("single",data,single,OutOfBand::Clamp);
    near(clamp.noise_correlation(0.).watts_per_hz(1,1)/kt,1.);
    rejects<std::invalid_argument>([&]{TabulatedNoiseModel bad("bad",data,NoiseTable{});});
    single.samples[0].watts_per_hz(0,0)=-kt;
    rejects<std::invalid_argument>([&]{TabulatedNoiseModel bad("bad",data,single);});
}
