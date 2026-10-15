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

std::string second_noise() {
    rfmodel::TouchstoneData data;
    data.ports = 2;
    data.reference_impedance_ohms = 75.;
    data.reference_impedances_ohms = {31., 73.};
    data.frequencies_hz = {1.0e9, 3.0e9};
    data.matrices = {{2, {{0., 0.}, {0.25, 0.}, {2., 0.}, {0.125, 0.}}},
                     {2, {{0., 0.}, {0.25, 0.}, {2., 0.}, {0.125, 0.}}}};
    data.noise_samples = {{1.5e9, 3., {0.5, 0.}, 37.5}, {4.0e9, 4., {0., 0.25}, 75.}};
    std::ostringstream stream;
    rfmodel::write_touchstone_v2(stream, data);
    return stream.str();
}
