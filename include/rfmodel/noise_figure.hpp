#pragma once
#include "noise_matrix.hpp"
namespace rfmodel {
// Forward (port 0 -> 1), noiseless matched output, passive source |Gamma|<1.
// Intrinsic covariance excludes the source and output termination noise.
inline double two_port_noise_figure_db(const SMatrix& s,const NoiseCorrelation& intrinsic,
                                       Complex source_reflection={},double reference_temperature_k=290.) {
    noise_detail::finite_matrix(s);
    if (s.ports!=2 || intrinsic.watts_per_hz.ports!=2)
        throw std::invalid_argument("noise figure requires a two-port");
    if (!std::isfinite(source_reflection.real()) || !std::isfinite(source_reflection.imag()) ||
        std::abs(source_reflection)>=1. || !std::isfinite(reference_temperature_k) || reference_temperature_k<=0)
        throw std::invalid_argument("invalid noise figure source/reference");
    if (s(1,0)==Complex{}) throw std::domain_error("zero forward transmission");
    const auto feedback=s(0,0)*source_reflection;
    const auto denominator=1.-feedback;
    if (!std::isfinite(feedback.real()) || !std::isfinite(feedback.imag()))
        throw std::overflow_error("noise figure feedback overflow");
    if (std::abs(denominator)<=64*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(feedback)))
        throw std::domain_error("singular noise figure source feedback");
    auto normalized=intrinsic;
    noise_detail::finite_matrix(normalized.watts_per_hz);
    constexpr double boltzmann=1.380649e-23;
    const double kt=boltzmann*reference_temperature_k;
    if (kt==0) throw std::overflow_error("noise reference underflow");
    for (auto& value:normalized.watts_per_hz.values) value/=kt;
    // Refer c2 back to the source plane; c1 is reflected by the source.
    const SMatrix weights{2,{source_reflection,denominator/s(1,0),0.,0.}};
    const double added=propagate_noise(weights,normalized).watts_per_hz(0,0).real();
    const double excess=added/(1.-std::norm(source_reflection));
    if (!std::isfinite(excess)) throw std::overflow_error("noise factor overflow");
    return 10./std::log(10.)*std::log1p(excess);
}
}
