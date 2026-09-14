#include "rfmodel/device_model.hpp"
#include <cassert>
int main(){ using namespace rfmodel; SMatrix s{2,{Complex{0,0},Complex{0.5,0},Complex{0,0},Complex{0,0}}}; StaticSParameterModel m("attenuator",{{0,"in"},{1,"out"}},s); assert(m.port_count()==2); assert(m.s_parameters(1e9)(0,1)==Complex{0.5,0}); }
