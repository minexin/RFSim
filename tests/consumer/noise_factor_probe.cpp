#include <rfmodel/noise/noise_matrix.hpp>
#include <rfmodel/noise/loaded_noise.hpp>
#include <rfmodel/noise/noise_renormalization.hpp>

rfmodel::NoiseCorrelation installed_noise_factor_probe() {
    return rfmodel::propagate_noise(rfmodel::SMatrix{2, {1., -.99, 100., -99.99}},
                                    {rfmodel::SMatrix{2, {1., 1., 1., 1.}}});
}

rfmodel::NoiseCorrelation installed_independent_noise_probe() {
    const rfmodel::SMatrix zero{2, {0., 0., 0., 0.}};
    const auto source = rfmodel::independent_noise({{rfmodel::SMatrix{2, {1., 0., 0., 1e-16}}}});
    const auto loaded = rfmodel::loaded_noise(zero, source, {0., 0.}, {zero});
    return rfmodel::renormalize_noise(zero, loaded.outgoing, 50., 50.);
}
