#pragma once
#include "touchstone.hpp"
#include <algorithm>
namespace rfmodel {
inline SMatrix interpolate_s(const TouchstoneData& d,double f){ if(d.matrices.empty()) throw std::invalid_argument("empty Touchstone data"); if(f<=d.frequencies_hz.front()) return d.matrices.front(); if(f>=d.frequencies_hz.back()) return d.matrices.back(); auto it=std::lower_bound(d.frequencies_hz.begin(),d.frequencies_hz.end(),f); auto i=std::size_t(it-d.frequencies_hz.begin()); double t=(f-d.frequencies_hz[i-1])/(d.frequencies_hz[i]-d.frequencies_hz[i-1]); SMatrix out{d.ports,std::vector<Complex>(d.ports*d.ports)}; for(std::size_t k=0;k<out.values.size();++k) out.values[k]=d.matrices[i-1].values[k]*(1-t)+d.matrices[i].values[k]*t; return out; }
}
