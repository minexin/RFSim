#include "rfmodel/network_parameters.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    near(s_to_z(SMatrix{1,{1./3}})(0,0),100.);
    near(s_to_y(SMatrix{1,{1./3}})(0,0),0.01);
    // Coupled reciprocal network S=[0,.5;.5,0].
    const SMatrix s{2,{0.,0.5,0.5,0.}};
    auto z=s_to_z(s,3.); auto y=s_to_y(s,3.);
    near(z(0,0),5.); near(z(0,1),4.); near(z(1,0),4.); near(z(1,1),5.);
    near(y(0,0),5./9); near(y(0,1),-4./9);
    // Nonreciprocal case protects against transpose and element-wise inversion.
    auto n=s_to_z(SMatrix{2,{0.,0.25,0.5,0.}},7.);
    near(n(0,0),9.); near(n(0,1),4.); near(n(1,0),8.);
    // Row swap required when I-S has zero leading diagonal.
    auto p=s_to_z(SMatrix{2,{1.,1.,1.,0.}},1.);
    near(p(0,0),-3.); near(p(0,1),-2.); near(p(1,1),-1.);
    rejects<std::domain_error>([]{s_to_z(SMatrix{1,{1.}});});
    rejects<std::domain_error>([]{s_to_y(SMatrix{1,{-1.}});});
    rejects<std::invalid_argument>([]{s_to_z(SMatrix{2,{0.}});});
    rejects<std::invalid_argument>([]{s_to_y(SMatrix{1,{0.}},-1.);});
}
