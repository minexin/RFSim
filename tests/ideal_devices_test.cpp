#include "rfmodel/ideal_devices.hpp"
#include "rfmodel/linear_analysis.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    MatchedTransmissionModel pad("10 dB pad",10);
    near(std::norm(pad.s_parameters(1e9)(1,0)),0.1);
    near(pad.s_parameters(0)(0,0),0.);
    MatchedTransmissionModel delay("delay",0,0.25e-9);
    near(delay.s_parameters(1e9)(1,0),Complex{0,-1});
    near(delay.s_parameters(2e9)(1,0),Complex{-1,0});
    near(delay.s_parameters(1e9)(0,1),delay.s_parameters(1e9)(1,0));
    auto result=analyze_linear(FrequencyGrid{{1e9,2e9}}, {0,3}, [&](double f) {
        LinearNetwork net;
        net.add(pad.s_parameters(f)); net.add(delay.s_parameters(f));
        net.connect(1,2); return net;
    });
    near(result.scattering[0](1,0),Complex{0,-std::sqrt(0.1)});
    near(result.scattering[1](1,0),Complex{-std::sqrt(0.1),0});
    MatchedTransmissionModel custom("75 ohm",0,0,75);
    near(custom.port(0).reference_impedance,75.);
    rejects<std::invalid_argument>([]{MatchedTransmissionModel bad("bad",-1);});
    rejects<std::invalid_argument>([]{MatchedTransmissionModel bad("bad",0,-1);});
    rejects<std::invalid_argument>([&]{delay.s_parameters(-1);});
    rejects<std::out_of_range>([&]{pad.port(2);});
}
