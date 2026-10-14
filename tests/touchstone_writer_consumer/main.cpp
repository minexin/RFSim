#include <rfmodel/io/touchstone_writer.hpp>
#include <sstream>
#include <cstdio>
#include <fstream>
#include <stdexcept>
std::string second();

int main() {
    rfmodel::TouchstoneData data;
    data.ports = 1;
    data.frequencies_hz = {0.};
    data.matrices = {{1, {{0.25, -0.5}}}};
    std::ostringstream stream;
    rfmodel::write_touchstone_v2(stream, data);
    if (stream.str() != second() ||
        stream.str().find("0 0.25 -0.5\n[End]\n") == std::string::npos) {
        throw std::runtime_error("installed two-TU export mismatch");
    }
    data.reference_impedance_ohms = 0x1p-1074;
    data.matrices[0].values[0] = {0x1p-1074, -0x1p-1074};
    {
        std::ofstream file("installed-numeric.ts");
        rfmodel::write_touchstone_v2(file, data);
    }
    const auto roundtrip = rfmodel::read_touchstone("installed-numeric.ts");
    std::remove("installed-numeric.ts");
    if (roundtrip.reference_impedance_ohms != 0x1p-1074 ||
        roundtrip.matrices[0].values[0] != data.matrices[0].values[0]) {
        throw std::runtime_error("installed subnormal roundtrip mismatch");
    }
}
