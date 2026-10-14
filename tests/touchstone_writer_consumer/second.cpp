#include <rfmodel/touchstone_writer.hpp>
#include <sstream>

std::string second() {
    rfmodel::TouchstoneData data;
    data.ports = 1;
    data.frequencies_hz = {0.};
    data.matrices = {{1, {{0.25, -0.5}}}};
    std::ostringstream stream;
    rfmodel::write_touchstone_v2(stream, data);
    return stream.str();
}
