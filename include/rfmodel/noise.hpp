#pragma once
#include "device_model.hpp"
#include <cmath>
namespace rfmodel {
struct NoiseParameters { double noise_figure_db{}; double reference_temperature_k{290.0}; double equivalent_temperature_k() const { return (std::pow(10.0,noise_figure_db/10.0)-1.0)*reference_temperature_k; } };
class NoiseProvider { public: virtual ~NoiseProvider() = default; virtual NoiseParameters noise_parameters(double frequency_hz) const = 0; };
struct NoiseStage { double gain_db{}; double noise_figure_db{}; };
inline double cascade_noise_figure_db(const std::vector<NoiseStage>& stages){ if(stages.empty()) return 0.0; double f=std::pow(10.0,stages.front().noise_figure_db/10.0), g=std::pow(10.0,stages.front().gain_db/10.0); for(std::size_t i=1;i<stages.size();++i){ f+=(std::pow(10.0,stages[i].noise_figure_db/10.0)-1.0)/g; g*=std::pow(10.0,stages[i].gain_db/10.0); } return 10.0*std::log10(f); }
}
