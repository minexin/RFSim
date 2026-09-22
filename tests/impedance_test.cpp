#include "rfmodel/measurements.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    near(input_impedance(0.),50.);
    near(input_impedance(1./3),100.);
    near(input_admittance(1./3),0.01);
    near(input_impedance(Complex{0,1}),Complex{0,50});
    near(input_admittance(Complex{0,1}),Complex{0,-0.02});
    near(input_impedance(-1),0.);
    near(input_admittance(1),0.);
    near(input_impedance(0.,75.),75.);
    rejects<std::domain_error>([]{input_impedance(1);});
    rejects<std::domain_error>([]{input_admittance(-1);});
    rejects<std::invalid_argument>([]{input_impedance(0.,0.);});
    // Matched thru terminated in 100 ohms presents 100 ohms at its input.
    LinearNetwork net;
    net.add(SMatrix{2,{0.,1.,1.,0.}});
    net.terminate(1,1./3);
    const auto s=net.external_s({0});
    near(input_impedance(s(0,0)),100.);
}
