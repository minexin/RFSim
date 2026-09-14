#pragma once
#include <cmath>
#include <complex>
#include <stdexcept>
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline void near(std::complex<double> actual, std::complex<double> expected) {
    require(std::abs(actual - expected) < 1e-12, "complex value mismatch");
}
template<class Exception, class Function> void rejects(Function action) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error("expected exception was not raised");
}
