#pragma once
#include "device_model.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
namespace rfmodel {
struct TouchstoneData { std::size_t ports{}; double frequency_scale{1.0}; std::vector<double> frequencies_hz; std::vector<SMatrix> matrices; };
inline TouchstoneData read_touchstone(const std::string& path){ std::ifstream in(path); if(!in) throw std::runtime_error("cannot open Touchstone file"); TouchstoneData d; std::string line; bool header=false; while(std::getline(in,line)){ if(line.empty()||line[0]=='!') continue; if(line[0]=='#'){ std::istringstream h(line); std::string hash,unit,param,format,ref; h>>hash>>unit>>param>>format; std::transform(unit.begin(),unit.end(),unit.begin(),::toupper); d.frequency_scale=(unit=="GHZ"?1e9:unit=="MHZ"?1e6:unit=="KHZ"?1e3:1.0); header=true; continue;} if(!header) continue; std::istringstream ss(line); std::vector<double> v; double x; while(ss>>x)v.push_back(x); if(v.size()<3) continue; if(!d.ports){ std::string ext=path.substr(path.find_last_of('.')+1); d.ports=std::stoul(ext.substr(1)); } const auto n=d.ports*d.ports; if(v.size()!=1+2*n) throw std::runtime_error("unsupported Touchstone row"); SMatrix m{d.ports,std::vector<Complex>(n)}; for(std::size_t i=0;i<n;++i)m.values[i]=Complex(v[1+2*i],v[2+2*i]); d.frequencies_hz.push_back(v[0]*d.frequency_scale); d.matrices.push_back(std::move(m)); } if(!header||d.ports==0||d.matrices.empty()) throw std::runtime_error("invalid Touchstone file"); return d; }
}
