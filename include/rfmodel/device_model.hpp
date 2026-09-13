#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>
namespace rfmodel {
using Complex = std::complex<double>;
struct PortInfo { std::size_t id{}; std::string name; Complex reference_impedance{50.0,0.0}; };
struct FrequencyGrid { std::vector<double> hz; };
class RFDeviceModel { public: virtual ~RFDeviceModel() = default; virtual std::string name() const = 0; virtual std::size_t port_count() const = 0; virtual PortInfo port(std::size_t) const = 0; };
class SParameterProvider { public: virtual ~SParameterProvider() = default; virtual std::vector<Complex> s_parameters(double frequency_hz) const = 0; };
}
