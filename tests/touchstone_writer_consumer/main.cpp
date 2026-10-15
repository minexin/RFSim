#include <rfmodel/io/touchstone_writer.hpp>
#include <sstream>
#include <cstdio>
#include <fstream>
#include <stdexcept>
std::string second();
std::string second_noise();

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

    rfmodel::TouchstoneData noise;
    noise.ports = 2;
    noise.reference_impedance_ohms = 75.;
    noise.reference_impedances_ohms = {31., 73.};
    noise.frequencies_hz = {1.0e9, 3.0e9};
    noise.matrices = {{2, {{0., 0.}, {0.25, 0.}, {2., 0.}, {0.125, 0.}}},
                      {2, {{0., 0.}, {0.25, 0.}, {2., 0.}, {0.125, 0.}}}};
    noise.noise_samples = {{1.5e9, 3., {0.5, 0.}, 37.5}, {4.0e9, 4., {0., 0.25}, 75.}};
    // Deliberately incompatible source representation: stored values are already SI/RI/ohms.
    noise.version = rfmodel::TouchstoneVersion::Legacy;
    noise.format = rfmodel::TouchstoneFormat::DB;
    noise.frequency_scale = 1.0e9;
    noise.matrix_format = rfmodel::TouchstoneMatrixFormat::Lower;
    std::ostringstream noisy_stream;
    rfmodel::write_touchstone_v2(noisy_stream, noise);
    const std::string expected =
        "[Version] 2.0\n# Hz S RI R 75\n[Number of Ports] 2\n"
        "[Two-Port Data Order] 12_21\n[Number of Frequencies] 2\n"
        "[Number of Noise Frequencies] 2\n[Reference] 31 73\n"
        "[Matrix Format] Full\n[Network Data]\n"
        "1000000000 0 0 0.25 0 2 0 0.125 0\n"
        "3000000000 0 0 0.25 0 2 0 0.125 0\n"
        "[Noise Data]\n1500000000 3 0.5 0 37.5\n4000000000 4 0.25 90 75\n[End]\n";
    if (noisy_stream.str() != expected || second_noise() != expected) {
        throw std::runtime_error("installed two-TU noise golden mismatch");
    }
    {
        std::ofstream file("installed-noise.ts");
        rfmodel::write_touchstone_v2(file, noise);
    }
    const auto restored_noise = rfmodel::read_touchstone("installed-noise.ts");
    std::remove("installed-noise.ts");
    if (restored_noise.noise_samples.size() != 2 ||
        restored_noise.reference_impedance_ohms != 75. ||
        restored_noise.reference_impedances_ohms != std::vector<double>{31., 73.} ||
        restored_noise.noise_samples[0].noise_resistance_ohms != 37.5 ||
        restored_noise.noise_samples[1].frequency_hz != 4.0e9) {
        throw std::runtime_error("installed noise reference or unit mismatch");
    }
    noise.noise_samples[0].noise_resistance_ohms = -1.;
    std::ostringstream rejected;
    rejected << "untouched";
    bool did_reject = false;
    try {
        rfmodel::write_touchstone_v2(rejected, noise);
    } catch (const std::invalid_argument &) {
        did_reject = true;
    }
    if (!did_reject || rejected.str() != "untouched") {
        throw std::runtime_error("installed invalid noise touched output");
    }
}
