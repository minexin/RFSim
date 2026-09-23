#include "rfmodel/noise.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    near(cascade_noise_figure_db({}), 0.0);
    // Exact linear factors: F1=2, F2=3, G1=10; total F=2.2.
    near(cascade_noise_figure_db({{10., 10 * std::log10(2.)}, {0., 10 * std::log10(3.)}}),
         10 * std::log10(2.2));
    near(NoiseParameters{10 * std::log10(2.), 290}.equivalent_temperature_k(), 290.);
    near(NoiseParameters{0, 290}.equivalent_temperature_k(), 0.);
    // A room-temperature 10 dB pad preceding a factor-2 amplifier: total F=20.
    near(cascade_noise_figure_db({{-10., 10.}, {20., 10 * std::log10(2.)}}), 10 * std::log10(20.));
    rejects<std::invalid_argument>([] {
        cascade_noise_figure_db({{0., -0.1}});
    });
    rejects<std::invalid_argument>([&] {
        cascade_noise_figure_db({{nan, 1.}});
    });
    rejects<std::invalid_argument>([&] {
        NoiseParameters{nan, 290}.equivalent_temperature_k();
    });
    rejects<std::invalid_argument>([] {
        NoiseParameters{1, 0}.equivalent_temperature_k();
    });
    rejects<std::invalid_argument>([] {
        NoiseParameters{-1, 290}.equivalent_temperature_k();
    });
    rejects<std::overflow_error>([] {
        NoiseParameters{4000, 290}.equivalent_temperature_k();
    });
    rejects<std::overflow_error>([] {
        cascade_noise_figure_db({{4000., 1.}});
    });
}
