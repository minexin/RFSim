#include "rfmodel/sweep.hpp"
#include "rfmodel/interpolation.hpp"
#include "test_support.hpp"
#include <limits>

int main(int argc, char **argv) {
    using namespace rfmodel;
    require(argc == 2, "fixture root");
    const auto data = read_touchstone(std::string(argv[1]) + "/sweep.s2p");
    auto build = [&](double f) {
        LinearNetwork net;
        net.add(interpolate_s(data, f), data.reference_impedance_ohms);
        net.terminate(0, 0., 1.);
        net.terminate(1, 0.5);
        return net;
    };
    const auto result = sweep_network(FrequencyGrid{{1e9, 2e9, 3e9}}, build);
    require(result.size() == 3, "sweep count");
    for (std::size_t i = 0; i < 3; ++i) {
        const double gain = 0.5 - 0.125 * i;
        near(result[i].waves.outgoing[1], gain);
        near(result[i].waves.outgoing[0], 0.5 * gain * gain);
        require(result[i].frequency_hz == (i + 1) * 1e9, "frequency label");
    }
    rejects<std::out_of_range>([&] {
        interpolate_s(data, 4e9);
    });
    near(interpolate_s(data, 4e9, OutOfBand::Clamp)(1, 0), 0.25);
    rejects<std::runtime_error>([&] {
        sweep_network(FrequencyGrid{{4e9}}, build);
    });
    auto invalid = data;
    invalid.frequencies_hz.clear();
    rejects<std::invalid_argument>([&] {
        interpolate_s(invalid, 1e9);
    });
    invalid = data;
    invalid.frequencies_hz[1] = 1e9;
    rejects<std::invalid_argument>([&] {
        interpolate_s(invalid, 1e9);
    });
    invalid = data;
    invalid.matrices[1].values.clear();
    rejects<std::invalid_argument>([&] {
        interpolate_s(invalid, 1e9);
    });
    rejects<std::invalid_argument>([&] {
        interpolate_s(data, std::numeric_limits<double>::quiet_NaN());
    });
    int calls = 0;
    rejects<std::invalid_argument>([&] {
        sweep_network(FrequencyGrid{{2., 1.}}, [&](double) {
            ++calls;
            return LinearNetwork{};
        });
    });
    require(calls == 0, "invalid grid must not run models");
}
