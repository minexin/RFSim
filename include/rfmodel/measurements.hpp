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
// Real positive reference only. Singular open/short limits are explicit errors.
inline Complex input_impedance(Complex reflection, double reference_ohms=50.0) {
    if (!std::isfinite(reference_ohms) || reference_ohms<=0 ||
        !std::isfinite(reflection.real()) || !std::isfinite(reflection.imag()))
        throw std::invalid_argument("invalid impedance conversion input");
    if (reflection==Complex{1,0}) throw std::domain_error("open circuit has infinite impedance");
    const Complex value=reference_ohms*((Complex{1,0}+reflection)/(Complex{1,0}-reflection));
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        throw std::overflow_error("impedance conversion overflow");
    return value;
}
inline Complex input_admittance(Complex reflection, double reference_ohms=50.0) {
    if (!std::isfinite(reference_ohms) || reference_ohms<=0 ||
        !std::isfinite(reflection.real()) || !std::isfinite(reflection.imag()))
        throw std::invalid_argument("invalid admittance conversion input");
    if (reflection==Complex{-1,0}) throw std::domain_error("short circuit has infinite admittance");
    const Complex value=((Complex{1,0}-reflection)/(Complex{1,0}+reflection))/reference_ohms;
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        throw std::overflow_error("admittance conversion overflow");
    return value;
}
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
