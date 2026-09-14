#pragma once
#include "device_model.hpp"
namespace rfmodel {
struct TwoPortWave { Complex a1{}, a2{}, b1{}, b2{}; };
inline TwoPortWave evaluate_two_port(const SMatrix& s, Complex source_incident, Complex load_incident = {}) {
 if(s.ports!=2) throw std::invalid_argument("two-port matrix required");
 TwoPortWave w; w.a1=source_incident; w.a2=load_incident; w.b1=s(0,0)*w.a1+s(0,1)*w.a2; w.b2=s(1,0)*w.a1+s(1,1)*w.a2; return w;
}
}
