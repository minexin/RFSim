#include "rfmodel/touchstone.hpp"
#include "test_support.hpp"
int main(){ using namespace rfmodel; auto d=read_touchstone("../tests/sample.s2p"); require(d.ports==2,"ports"); require(d.frequencies_hz[1]==2e9,"GHz scale"); near(d.matrices[0](0,0),0.0); }
