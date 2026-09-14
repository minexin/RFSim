#include "rfmodel/touchstone.hpp"
#include "test_support.hpp"
#include <string>
int main(int argc, char** argv) {
    using namespace rfmodel;
    require(argc == 2, "fixture directory argument required");
    const std::string fixtures = std::string(argv[1]) + "/";
    auto d = read_touchstone(fixtures + "sample.s2p");
    require(d.ports == 2, "ports");
    require(d.frequencies_hz.size() == 2, "frequency count");
    require(d.frequencies_hz[1] == 2e9, "GHz scale");
    near(d.matrices[0](0,0), 0.0);
    d = read_touchstone(fixtures + "magnitude.s1p");
    require(d.format == TouchstoneFormat::MA, "MA format");
    require(d.frequencies_hz[0] == 1e9, "MHz scale");
    near(d.matrices[0](0,0), Complex{0,0.5});
    d = read_touchstone(fixtures + "decibel.s1p");
    require(d.format == TouchstoneFormat::DB, "DB format");
    require(d.frequencies_hz[0] == 1e9, "kHz scale");
    near(d.matrices[0](0,0), Complex{0,-0.1});
    rejects<std::runtime_error>([&] {
        read_touchstone(fixtures + "invalid-format.s1p");
    });
}
