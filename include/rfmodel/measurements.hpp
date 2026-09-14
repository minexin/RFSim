#pragma once
#include "device_model.hpp"
#include <cmath>
#include <limits>
namespace rfmodel {
struct PortPower {
    double incident_w;
    double outgoing_w;
    double absorbed_w;
};
inline PortPower port_power(Complex incident, Complex outgoing) {
    const double a=std::norm(incident), b=std::norm(outgoing);
    if (!std::isfinite(a) || !std::isfinite(b)) throw std::invalid_argument("nonfinite wave power");
    return {a,b,a-b};
}
inline double watts_to_dbm(double watts) {
    if (!std::isfinite(watts) || watts<0) throw std::invalid_argument("invalid power");
    return watts==0 ? -std::numeric_limits<double>::infinity() : 10*std::log10(watts)+30;
}
inline double dbm_to_watts(double dbm) {
    if (dbm==-std::numeric_limits<double>::infinity()) return 0;
    if (!std::isfinite(dbm)) throw std::invalid_argument("invalid dBm");
    const double watts=std::pow(10.,(dbm-30)/10);
    if (!std::isfinite(watts)) throw std::overflow_error("power overflow");
    return watts;
}
inline double power_gain_db(double output_w, double input_w) {
    if (!std::isfinite(input_w) || input_w<=0) throw std::invalid_argument("positive input power required");
    return watts_to_dbm(output_w)-watts_to_dbm(input_w);
}
inline double return_loss_db(Complex reflection) {
    const double magnitude=std::abs(reflection);
    if (!std::isfinite(magnitude)) throw std::invalid_argument("invalid reflection");
    return magnitude==0 ? std::numeric_limits<double>::infinity() : -20*std::log10(magnitude);
}
// Passive reflection only; active |Gamma|>1 has no conventional VSWR here.
inline double vswr(Complex reflection) {
    const double magnitude=std::abs(reflection);
    if (!std::isfinite(magnitude) || magnitude>1) throw std::invalid_argument("VSWR requires |Gamma|<=1");
    return magnitude==1 ? std::numeric_limits<double>::infinity() : (1+magnitude)/(1-magnitude);
}
}
