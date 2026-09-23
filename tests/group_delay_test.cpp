#include "rfmodel/group_delay.hpp"
#include "rfmodel/ideal_devices.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    FrequencyGrid grid{{0., 1e8, 2.5e8, 4e8, 5.5e8}};
    MatchedTransmissionModel model("delay", 10., 2e-9);
    std::vector<Complex> response;
    for (double f : grid.hz) {
        response.push_back(model.s_parameters(f)(1, 0));
    }
    const auto delays = group_delay(grid, response);
    require(delays.size() == 4, "interval count");
    for (auto d : delays) {
        require(std::abs(d.delay_seconds - 2e-9) < 1e-22, "constant delay across phase wrap");
    }
    near(delays[1].center_frequency_hz, 1.75e8);
    near(delays[1].aperture_hz, 1.5e8);
    // Quadratic phase has an exact secant derivative at each midpoint.
    constexpr double pi = 3.14159265358979323846;
    response.clear();
    for (double f : grid.hz) {
        response.push_back(std::polar(1., -pi * 1e-18 * f * f));
    }
    auto quadratic = group_delay(grid, response);
    for (auto d : quadratic) {
        require(std::abs(d.delay_seconds - 1e-18 * d.center_frequency_hz) < 1e-22,
                "quadratic midpoint derivative");
    }
    rejects<std::domain_error>([] {
        group_delay(FrequencyGrid{{0., 1.}}, {1., 0.});
    });
    rejects<std::domain_error>([] {
        group_delay(FrequencyGrid{{0., 1.}}, {1., -1.});
    });
    rejects<std::invalid_argument>([] {
        group_delay(FrequencyGrid{{1., 1.}}, {1., 1.});
    });
    rejects<std::invalid_argument>([] {
        group_delay(FrequencyGrid{{1., 2.}}, {1.});
    });
    rejects<std::invalid_argument>([] {
        group_delay(FrequencyGrid{{1., 2.}}, {1., {std::numeric_limits<double>::quiet_NaN(), 0}});
    });
}
