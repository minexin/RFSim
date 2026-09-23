#pragma once
#include "device_model.hpp"
#include <cmath>
namespace rfmodel {
struct GroupDelayInterval {
    double center_frequency_hz;
    double aperture_hz;
    double delay_seconds;
};
// Adjacent-sample phase slope. N samples produce N-1 midpoint results.
// Requires phase change strictly less than pi between samples; aliasing cannot
// be detected from sampled data alone. Zero response has undefined phase.
inline std::vector<GroupDelayInterval> group_delay(
    const FrequencyGrid& grid, const std::vector<Complex>& response) {
    if (grid.hz.size()<2 || grid.hz.size()!=response.size())
        throw std::invalid_argument("group delay needs at least two corresponding samples");
    constexpr double pi=3.14159265358979323846;
    for (std::size_t i=0; i<response.size(); ++i) {
        if (!std::isfinite(grid.hz[i]) || grid.hz[i]<0 || (i && grid.hz[i]<=grid.hz[i-1]))
            throw std::invalid_argument("invalid group delay frequency grid");
        if (!std::isfinite(response[i].real()) || !std::isfinite(response[i].imag()))
            throw std::invalid_argument("nonfinite frequency response");
        if (response[i]==Complex{}) throw std::domain_error("group delay undefined at zero response");
    }
    std::vector<GroupDelayInterval> result;
    result.reserve(response.size()-1);
    for (std::size_t i=1; i<response.size(); ++i) {
        const double aperture=grid.hz[i]-grid.hz[i-1];
        const double phase=std::remainder(std::arg(response[i])-std::arg(response[i-1]),2*pi);
        if (std::abs(phase)==pi) throw std::domain_error("ambiguous half-turn phase increment");
        const double delay=-(phase/(2*pi))/aperture;
        if (!std::isfinite(delay)) throw std::overflow_error("group delay overflow");
        result.push_back({grid.hz[i-1]+aperture/2,aperture,delay});
    }
    return result;
}
}
