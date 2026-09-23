#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rfmodel {
namespace parameter_detail {
// Solve A*X=B with partial pivoting; multiple right-hand sides.
inline SMatrix solve(SMatrix a, SMatrix b) {
    const auto n=a.ports;
    double scale=0;
    for (std::size_t r=0;r<n;++r) {
        double sum=0;
        for (std::size_t c=0;c<n;++c) sum+=std::abs(a(r,c));
        scale=std::max(scale,sum);
    }
    const double threshold=64*std::numeric_limits<double>::epsilon()*n*scale;
    for (std::size_t k=0;k<n;++k) {
        std::size_t pivot=k;
        for (std::size_t r=k+1;r<n;++r) if (std::abs(a(r,k))>std::abs(a(pivot,k))) pivot=r;
        if (std::abs(a(pivot,k))<=threshold) throw std::domain_error("singular parameter conversion");
        for (std::size_t c=0;c<n;++c) { std::swap(a(k,c),a(pivot,c)); std::swap(b(k,c),b(pivot,c)); }
        for (std::size_t r=k+1;r<n;++r) {
            const auto factor=a(r,k)/a(k,k);
            a(r,k)=0;
            for (std::size_t c=k+1;c<n;++c) a(r,c)-=factor*a(k,c);
            for (std::size_t c=0;c<n;++c) b(r,c)-=factor*b(k,c);
        }
    }
    for (std::size_t r=n;r-- >0;)
        for (std::size_t col=0;col<n;++col) {
            for (std::size_t c=r+1;c<n;++c) b(r,col)-=a(r,c)*b(c,col);
            b(r,col)/=a(r,r);
        }
    return b;
}
inline SMatrix convert(const SMatrix& s,double reference,bool admittance) {
    if (!s.ports || s.ports>1024 || s.values.size()!=s.ports*s.ports ||
        !std::isfinite(reference) || reference<=0) throw std::invalid_argument("invalid conversion dimensions/reference");
    for (auto x:s.values) if (!std::isfinite(x.real()) || !std::isfinite(x.imag()))
        throw std::invalid_argument("nonfinite S parameter");
    SMatrix a{s.ports,std::vector<Complex>(s.values.size())}, b=a;
    for (std::size_t r=0;r<s.ports;++r)
        for (std::size_t c=0;c<s.ports;++c) {
            const Complex identity=r==c?1.:0.;
            a(r,c)=identity+(admittance?s(r,c):-s(r,c));
            b(r,c)=identity+(admittance?-s(r,c):s(r,c));
        }
    auto result=solve(a,b);
    for (auto& x:result.values) {
        x=admittance?x/reference:x*reference;
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) throw std::overflow_error("parameter conversion overflow");
    }
    return result;
}
}
// SMatrix is used as a dense complex matrix container; output units differ.
inline SMatrix s_to_z(const SMatrix& s,double reference_ohms=50.) {
    return parameter_detail::convert(s,reference_ohms,false);
}
inline SMatrix s_to_y(const SMatrix& s,double reference_ohms=50.) {
    return parameter_detail::convert(s,reference_ohms,true);
}
}
