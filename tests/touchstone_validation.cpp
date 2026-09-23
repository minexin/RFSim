#include "rfmodel/touchstone.hpp"
#include "rfmodel/linear_solver.hpp"
#include "test_support.hpp"
#include <fstream>
#include <cstdio>

int main(int argc, char **argv) {
    using namespace rfmodel;
    require(argc == 2, "fixture directory");
    const std::string root = std::string(argv[1]) + "/";
    auto d = read_touchstone(root + "nonreciprocal.s2p");
    require(d.reference_impedance_ohms == 75, "reference resistance");
    near(d.matrices[0](1, 0), 2.0);
    near(d.matrices[0](0, 1), Complex{0, 0.3});
    near(evaluate_two_port(d.matrices[0], 1.0).b2, 2.0);
    d = read_touchstone(root + "three-port.s3p");
    near(d.matrices[0](1, 0), 4.0);
    near(d.matrices[0](0, 1), 2.0);
    near(d.matrices[0](2, 2), 9.0);
    const char *scratch = "touchstone-validation.s1p";
    auto write = [&](const std::string &data) {
        std::ofstream out(scratch);
        out << data;
    };
    write("#\n1 0.5 90\n");
    d = read_touchstone(scratch);
    require(d.frequencies_hz[0] == 1e9, "default GHz");
    near(d.matrices[0](0, 0), Complex{0, 0.5});
    for (const auto &data : {"# THZ S RI R 50\n1 0 0\n",
                             "# Hz Z RI R 50\n1 0 0\n",
                             "# Hz S RI R 0\n1 0 0\n",
                             "# Hz S RI R 50\n1 0 0\n1 0 0\n",
                             "# Hz S RI R 50\n1 0\n",
                             "# Hz S RI R 50\n1 NaN 0\n",
                             "# Hz S RI R 50\n1 0bad 0\n",
                             "# Hz S RI R 50\n1 0 0 garbage\n",
                             "[Version] 2.0\n# Hz S RI R 50\n1 0 0\n",
                             "1 0 0\n",
                             "# Hz S RI R 50\n-1 0 0\n",
                             "# Hz S MA R 50\n1 -1 0\n",
                             "# Hz S DB R 50\n1 99999 0\n",
                             "# Hz S RI R 50\n#\n1 0 0\n"}) {
        write(data);
        rejects<std::runtime_error>([&] {
            read_touchstone(scratch);
        });
    }
    rejects<std::runtime_error>([] {
        read_touchstone("invalid.s2pBAD");
    });
    std::remove(scratch);
}
