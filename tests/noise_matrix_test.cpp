#include "rfmodel/noise_matrix.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    constexpr double kt=1.380649e-23*290.;
    const auto pad=passive_thermal_noise(SMatrix{2,{0.,0.5,0.5,0.}},290.).watts_per_hz;
    near(pad(0,0)/kt,0.75); near(pad(1,1)/kt,0.75); near(pad(0,1)/kt,0.);
    IdealRLCModel r("R",IdealElement::Resistor,LumpedConnection::SeriesImpedance,50.);
    const auto thermal=passive_thermal_noise(r.s_parameters(1e9),290.).watts_per_hz;
    near(thermal(0,0)/kt,4./9); near(thermal(0,1)/kt,-4./9);
    IdealRLCModel l("L",IdealElement::Inductor,LumpedConnection::SeriesImpedance,1e-9);
    const auto lossless=passive_thermal_noise(l.s_parameters(1e9),290.).watts_per_hz;
    for (auto x:lossless.values) near(x/kt,0.);
    // Correlated rank-one source [1,i]; phase rotation [1,-i] gives [1,1].
    const NoiseCorrelation source{SMatrix{2,{1.,Complex{0.,-1.},Complex{0.,1.},1.}}};
    const auto transformed=propagate_noise(SMatrix{2,{1.,0.,0.,Complex{0.,-1.}}},source).watts_per_hz;
    for (auto x:transformed.values) near(x,1.);
    // Pivoting: large variance at the second port.
    const auto pivot=propagate_noise(SMatrix{2,{0.,1.,1.,0.}},NoiseCorrelation{SMatrix{2,{0.,0.,0.,4.}}}).watts_per_hz;
    near(pivot(0,0),4.); near(pivot(1,1),0.);
    rejects<std::invalid_argument>([]{passive_thermal_noise(SMatrix{1,{2.}},0.);});
    rejects<std::invalid_argument>([]{propagate_noise(SMatrix{2,{1.,0.,0.,1.}},NoiseCorrelation{SMatrix{2,{1.,2.,2.,1.}}});});
    rejects<std::invalid_argument>([]{propagate_noise(SMatrix{2,{1.,0.,0.,1.}},NoiseCorrelation{SMatrix{2,{1.,Complex{0.,1.},Complex{0.,1.},1.}}});});
    rejects<std::invalid_argument>([]{passive_thermal_noise(SMatrix{1,{0.}},-1.);});
}
