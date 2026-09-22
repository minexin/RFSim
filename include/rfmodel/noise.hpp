#pragma once
#include "device_model.hpp"
#include <cmath>
#include <limits>
namespace rfmodel {
struct NoiseParameters {
    double noise_figure_db{};
    double reference_temperature_k{290.0};
    double equivalent_temperature_k() const {
        if (!std::isfinite(noise_figure_db) || noise_figure_db<0 || !std::isfinite(reference_temperature_k) || reference_temperature_k<=0)
            throw std::invalid_argument("invalid noise parameters");
        const double temperature=std::expm1(noise_figure_db*(std::log(10.0)/10.0))*reference_temperature_k;
        if (!std::isfinite(temperature)) throw std::overflow_error("noise temperature overflow");
        return temperature;
    }
};
class NoiseProvider { public: virtual ~NoiseProvider() = default; virtual NoiseParameters noise_parameters(double frequency_hz) const = 0; };
struct NoiseStage { double gain_db{}; double noise_figure_db{}; };
inline double cascade_noise_figure_db(const std::vector<NoiseStage>& stages){
    if(stages.empty()) return 0.0;
    double f=1.0, gain=1.0;
    for(const auto& stage: stages){
        if(!std::isfinite(stage.gain_db)||!std::isfinite(stage.noise_figure_db)) throw std::invalid_argument("invalid noise stage");
        const double factor=std::pow(10.0,stage.noise_figure_db/10.0);
        if(factor<1.0) throw std::invalid_argument("noise figure below 0 dB");
        f+=(factor-1.0)/gain;
        gain*=std::pow(10.0,stage.gain_db/10.0);
        if(!std::isfinite(f)||!std::isfinite(gain)||gain<=0) throw std::overflow_error("noise cascade overflow");
    }
    return 10.0*std::log10(f);
}
}
