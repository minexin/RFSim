#include <rfmodel/io/touchstone_writer.hpp>
#include <sstream>
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
}
