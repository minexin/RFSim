#pragma once
#include "tabulated_model.hpp"
#include "noise_matrix.hpp"

namespace rfmodel {
struct NoiseTable {
    std::vector<double> frequencies_hz;
    std::vector<NoiseCorrelation> samples;
};

class TabulatedNoiseModel final : public RFDeviceModel,
                                  public SParameterProvider,
                                  public NoiseCorrelationProvider {
    TabulatedSParameterModel scattering_;
    NoiseTable noise_;
    OutOfBand policy_;

public:
    // Noise and S tables may use different grids but must use the same power
    // wave reference and port order. Both datasets are owned by this model.
    TabulatedNoiseModel(std::string name,
                        TouchstoneData scattering,
                        NoiseTable noise,
                        OutOfBand policy = OutOfBand::Reject)
        : scattering_(std::move(name), std::move(scattering), policy), noise_(std::move(noise)),
          policy_(policy) {
        if (policy != OutOfBand::Reject && policy != OutOfBand::Clamp) {
            throw std::invalid_argument("invalid table range policy");
        }
        if (noise_.samples.empty() || noise_.samples.size() != noise_.frequencies_hz.size()) {
            throw std::invalid_argument("inconsistent noise table");
        }
        for (std::size_t i = 0; i < noise_.samples.size(); ++i) {
            const double f = noise_.frequencies_hz[i];
            if (!std::isfinite(f) || f < 0 || (i && f <= noise_.frequencies_hz[i - 1])) {
                throw std::invalid_argument("invalid noise frequency grid");
            }
            auto &matrix = noise_.samples[i].watts_per_hz;
            if (matrix.ports != scattering_.port_count()) {
                throw std::invalid_argument("noise/S port count differs");
            }
            matrix = noise_detail::psd_matrix(matrix);
        }
    }

    std::string name() const override {
        return scattering_.name();
    }

    std::size_t port_count() const override {
        return scattering_.port_count();
    }

    PortInfo port(std::size_t i) const override {
        return scattering_.port(i);
    }

    SMatrix s_parameters(double f) const override {
        return scattering_.s_parameters(f);
    }

    NoiseCorrelation noise_correlation(double f) const override {
        if (!std::isfinite(f) || f < 0) {
            throw std::invalid_argument("invalid noise query frequency");
        }
        const auto &grid = noise_.frequencies_hz;
        if (f < grid.front() || f > grid.back()) {
            if (policy_ == OutOfBand::Reject) {
                throw std::out_of_range("frequency outside noise range");
            }
            return f < grid.front() ? noise_.samples.front() : noise_.samples.back();
        }
        const auto it = std::lower_bound(grid.begin(), grid.end(), f);
        const auto i = static_cast<std::size_t>(it - grid.begin());
        if (*it == f) {
            return noise_.samples[i];
        }
        const double t = (f - grid[i - 1]) / (grid[i] - grid[i - 1]);
        auto result = noise_.samples[i - 1];
        for (std::size_t k = 0; k < result.watts_per_hz.values.size(); ++k) {
            result.watts_per_hz.values[k] = (1 - t) * noise_.samples[i - 1].watts_per_hz.values[k] +
                                            t * noise_.samples[i].watts_per_hz.values[k];
        }
        return result; // Convex combination preserves Hermitian PSD structure.
    }

    double minimum_noise_frequency_hz() const {
        return noise_.frequencies_hz.front();
    }

    double maximum_noise_frequency_hz() const {
        return noise_.frequencies_hz.back();
    }
};
} // namespace rfmodel
