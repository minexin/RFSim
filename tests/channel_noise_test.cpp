#include "rfmodel/channel_noise.hpp"
#include "test_support.hpp"
#include <iostream>

int run() {
    using namespace rfmodel;
    const auto result = measure_channel_noise(
        {{0., 1e-20}, {10., 3e-20}}, {{3., 1.}, {5., 2.}, {7., 3.}, {8., 100.}}, 5., 4.);
    near(result.noise_power_w / 1e-20, 8.);
    near(result.desired_signal_power_w, 6.);
    near(result.lower_frequency_hz, 3.);
    near(result.upper_frequency_hz, 7.);
    near(*result.carrier_to_noise_db, 10. * (std::log10(6.) - std::log10(8e-20)));
    const auto dc = measure_channel_noise({{0., 2.}, {1., 2.}}, {{0., 1.}}, 0., 1.);
    near(dc.effective_bandwidth_hz, .5);
    near(dc.noise_power_w, 1.);
    near(dc.mean_noise_density_w_per_hz, 2.);
    const auto quiet = measure_channel_noise({{0., 0.}, {1., 0.}}, {{.5, 1.}}, .5, 1.);
    if (quiet.carrier_to_noise_db || quiet.ratio_state != ChannelRatioState::noise_free) {
        throw std::runtime_error("zero-noise status");
    }
    // A finite integral must not overflow while averaging two large endpoints.
    const auto large = measure_channel_noise({{0., 1e308}, {1., 1e308}}, {}, .5, 1.);
    near(large.noise_power_w / 1e308, 1.);
    const double tiny = std::numeric_limits<double>::denorm_min();
    const auto small = measure_channel_noise({{0., tiny}, {100., tiny}}, {}, 50., 100.);
    if (small.noise_power_w != 100. * tiny) {
        throw std::runtime_error("subnormal density lost before integration");
    }
    rejects<std::out_of_range>([] {
        measure_channel_noise({{1., 1.}, {2., 1.}}, {}, 1., 1.);
    });
    rejects<std::invalid_argument>([] {
        measure_channel_noise({{1., 1.}, {1., 1.}}, {}, 1., 1.);
    });
    rejects<std::invalid_argument>([] {
        measure_channel_noise({{0., -1.}, {1., 1.}}, {}, .5, 1.);
    });
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
