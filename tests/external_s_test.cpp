#include "rfmodel/network.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    LinearNetwork network;
    const SMatrix a{2,{0.1,0.2,2.0,0.3}};
    const SMatrix b{2,{0.4,0.5,3.0,0.6}};
    network.add(a); network.add(b); network.connect(1,2);
    auto s=network.external_s({0,3});
    // Analytic two-port cascade including all repeated internal reflections.
    const double denominator=1-0.3*0.4;
    near(s(0,0),0.1+0.2*0.4*2/denominator);
    near(s(0,1),0.2*0.5/denominator);
    near(s(1,0),3*2/denominator);
    near(s(1,1),0.6+3*0.3*0.5/denominator);
    auto reverse=network.external_s({3,0});
    near(reverse(0,0),s(1,1)); near(reverse(0,1),s(1,0));
    // Extraction did not mutate original boundary conditions.
    near(network.external_s({0,3})(1,0),s(1,0));
    rejects<std::invalid_argument>([&]{network.external_s({0,0});});
    rejects<std::invalid_argument>([&]{network.external_s({0});});
    rejects<std::out_of_range>([&]{network.external_s({99});});
    LinearNetwork terminated;
    terminated.add(a); terminated.terminate(1,0.5);
    near(terminated.external_s({0})(0,0),0.1+0.2*0.5*2/(1-0.3*0.5));
    LinearNetwork driven;
    driven.add(a); driven.terminate(1,0.,1.);
    rejects<std::invalid_argument>([&]{driven.external_s({0});});
}
