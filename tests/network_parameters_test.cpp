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
    // Cancellation at the identity scale must not yield a huge unstable result.
    rejects<std::domain_error>([]{s_to_z(SMatrix{1,{std::nextafter(1.,0.)}});});
    rejects<std::domain_error>([]{s_to_y(SMatrix{1,{std::nextafter(-1.,0.)}});});
    const SMatrix complex_network{3,{
        Complex{0.1,0.2},0.2,Complex{0.,0.1},
        0.4,Complex{-0.1,0.1},0.1,
        Complex{0.2,-0.1},0.3,-0.2}};
    const auto zn=s_to_z(complex_network),yn=s_to_y(complex_network);
    for (std::size_t r=0;r<3;++r)
        for (std::size_t c=0;c<3;++c) {
            Complex product=0.;
            for (std::size_t k=0;k<3;++k) product+=zn(r,k)*yn(k,c);
            near(product,r==c?1.:0.);
        }
}
