#include "rfmodel/linear_solver.hpp"
#include <cassert>
int main(){using namespace rfmodel; SMatrix s{2,{Complex{0,0},Complex{0.5,0},Complex{0,0},Complex{0,0}}}; auto w=evaluate_two_port(s,{1,0}); assert(w.b2==Complex{0,0}); assert(w.b1==Complex{0,0.5});}
