#include "rfmodel/tabulated_noise_model.hpp"
#include "test_support.hpp"
#include <cstdio>
#include <fstream>

int main() {
    using namespace rfmodel;
    const char *path = "touchstone-noise.s2p";
    const std::string network = "# MHz S RI R 75\n1000 0 0 2 0 0 0 0 0\n3000 0 0 2 0 0 0 0 0\n";
    auto write = [&](const std::string &content) {
        std::ofstream output(path);
        output << content;
    };
    write(network +
          "! Noise always uses magnitude/degrees\n1000 3 .5 90 2\n3000 3 .5 -90 2 ! tail\n");
    const auto data = read_touchstone(path);
    require(data.matrices.size() == 2 && data.noise_samples.size() == 2, "separate table sizes");
    near(data.noise_samples[0].frequency_hz, 1e9);
    near(data.noise_samples[0].optimum_source_reflection, Complex{0., .5});
    near(data.noise_samples[0].noise_resistance_ohms, 150.);
    const auto model = TabulatedNoiseModel::from_touchstone("amplifier", path);
    for (double f : {1e9, 3e9}) {
        const auto parameters =
            extract_noise_parameters(model.s_parameters(f), model.noise_correlation(f), 75.);
        near(parameters.minimum_noise_figure_db, 3.);
        near(parameters.noise_resistance_ohms, 150.);
        near(parameters.optimum_source_reflection, Complex{0., f == 1e9 ? .5 : -.5});
    }
    // Equal first noise/last network frequency is also a legal transition.
    write(network + "3000 3 0 0 2\n");
    require(read_touchstone(path).noise_samples.size() == 1, "equal-frequency transition");
    for (const auto &tail : {"1000 3 .5 90\n",
                             "1000 3 .5 90 2 7\n",
                             "1000 3 .5 90 2\n1000 3 .5 90 2\n",
                             "1000 3 -.5 90 2\n",
                             "1000 3 .5 90 -2\n",
                             "1000 3 .5 90 1e308\n",
                             "1000 3 .5 90 2\n4000 0 0 2 0 0 0 0 0\n"}) {
        write(network + tail);
        rejects<std::runtime_error>([&] {
            read_touchstone(path);
        });
    }
    write(network);
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_touchstone("missing", path);
    });
    // Syntactically valid but physically inconsistent noise parameters.
    write(network + "1000 3 0 0 0\n");
    require(read_touchstone(path).noise_samples.size() == 1, "raw parameters retained");
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_touchstone("invalid", path);
    });
    std::remove(path);
}
