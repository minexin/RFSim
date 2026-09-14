#include "rfmodel/network.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    const SMatrix thru{2,{0.,1.,1.,0.}};
    LinearNetwork load;
    load.add(thru);
    load.terminate(0,0.25,1.0);
    load.terminate(1,0.5);
    const auto w=load.solve();
    near(w.incident[0],8./7.);
    near(w.outgoing[0],4./7.);
    near(w.outgoing[1],8./7.);
    require(w.relative_residual<1e-12,"residual");
    LinearNetwork chain;
    chain.add(SMatrix{2,{0.,0.5,0.5,0.}});
    chain.add(SMatrix{2,{0.,0.2,0.2,0.}});
    chain.connect(1,2);
    chain.terminate(0,0.,1.);
    chain.terminate(3);
    near(chain.solve().outgoing[3],0.1);
    // Ideal three-port equal-impedance junction: verifies a non-chain topology.
    LinearNetwork junction;
    junction.add(SMatrix{3,{-1./3,2./3,2./3,2./3,-1./3,2./3,2./3,2./3,-1./3}});
    junction.terminate(0,0.,1.);
    junction.terminate(1);
    junction.terminate(2);
    auto j=junction.solve();
    near(j.outgoing[0],-1./3);
    near(j.outgoing[1],2./3);
    near(j.outgoing[2],2./3);
    LinearNetwork singular;
    singular.add(thru);
    singular.connect(0,1);
    rejects<std::runtime_error>([&]{singular.solve();});
    rejects<std::invalid_argument>([&]{chain.terminate(0);});
    rejects<std::invalid_argument>([&]{chain.add(thru,75.);});
    LinearNetwork open;
    open.add(thru);
    rejects<std::invalid_argument>([&]{open.solve();});
}
