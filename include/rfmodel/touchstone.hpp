#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
namespace rfmodel {
enum class TouchstoneFormat { RI, MA, DB };
struct TouchstoneData { std::size_t ports{}; double frequency_scale{1.0}; TouchstoneFormat format{TouchstoneFormat::RI}; std::vector<double> frequencies_hz; std::vector<SMatrix> matrices; };
inline TouchstoneData read_touchstone(const std::string& path) {
    std::ifstream in(path); if (!in) throw std::runtime_error("cannot open Touchstone file");
    TouchstoneData d; std::string line; bool header=false;
    while (std::getline(in,line)) {
        if (line.empty() || line[0]=='!') continue;
        if (line[0]=='#') { std::istringstream h(line); std::string hash,unit,param,format; h>>hash>>unit>>param>>format; std::transform(unit.begin(),unit.end(),unit.begin(),[](unsigned char c){return char(std::toupper(c));}); std::transform(format.begin(),format.end(),format.begin(),[](unsigned char c){return char(std::toupper(c));}); d.frequency_scale=unit=="GHZ"?1e9:unit=="MHZ"?1e6:unit=="KHZ"?1e3:1.0; if(format=="MA") d.format=TouchstoneFormat::MA; else if(format=="DB") d.format=TouchstoneFormat::DB; else if(format!="RI") throw std::runtime_error("unsupported Touchstone format"); header=true; continue; }
        if(!header) continue; std::istringstream ss(line); std::vector<double> v; double x; while(ss>>x)v.push_back(x); if(v.size()<3) continue;
        if(!d.ports){ auto dot=path.find_last_of('.'); if(dot==std::string::npos) throw std::runtime_error("missing Touchstone extension"); d.ports=std::stoul(path.substr(dot+2)); }
        const auto n=d.ports*d.ports; if(v.size()!=1+2*n) throw std::runtime_error("unsupported Touchstone row"); SMatrix m{d.ports,std::vector<Complex>(n)};
        for(std::size_t i=0;i<n;++i){ double a=v[1+2*i], b=v[2+2*i]; if(d.format==TouchstoneFormat::MA) m.values[i]=std::polar(a,b*3.14159265358979323846/180.0); else if(d.format==TouchstoneFormat::DB) m.values[i]=std::polar(std::pow(10.0,a/20.0),b*3.14159265358979323846/180.0); else m.values[i]=Complex(a,b); }
        d.frequencies_hz.push_back(v[0]*d.frequency_scale); d.matrices.push_back(std::move(m));
    }
    if(!header||d.ports==0||d.matrices.empty()) throw std::runtime_error("invalid Touchstone file"); return d;
}
}
