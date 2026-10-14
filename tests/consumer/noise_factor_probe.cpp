#include <rfmodel/noise/noise_matrix.hpp>

rfmodel::NoiseCorrelation installed_noise_factor_probe() {
    return rfmodel::propagate_noise(rfmodel::SMatrix{2, {1., -.99, 100., -99.99}},
                                    {rfmodel::SMatrix{2, {1., 1., 1., 1.}}});
}
