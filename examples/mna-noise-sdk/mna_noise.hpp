#pragma once
#include <rfmodel/adapters/model_adapter.hpp>
#include <rfmodel/noise/loaded_noise.hpp>
#include <array>

namespace mna_noise_example {
struct Circuit {
    double resistance_ohm = 5.;
    double inductance_h = 1. / (3.14159265358979323846 * 1e6);
    double temperature_k = 290.;
    std::array<rfmodel::Complex, 2> references_ohm{{{4., 3.}, {9., -2.}}};
};

enum class Noise {
    thermal,
    missing,
    explicit_zero
};

// Owns the finalized, source-free series RL circuit, port order, references and
// callbacks. No builder, Circuit or adapter object must survive its return.
rfmodel::ModelAdapter make_model(Circuit circuit, Noise noise = Noise::thermal);

struct Sample {
    double frequency_hz;
    rfmodel::SMatrix scattering;
    rfmodel::NoiseCorrelation intrinsic_noise;
    rfmodel::LoadedNoiseResult equilibrium;
    rfmodel::Complex load_voltage_v, load_current_a;
    double resistor_power_w;
    rfmodel::SMatrix cascade_scattering;
    rfmodel::NoiseCorrelation cascade_noise;
};

struct Result {
    std::vector<Sample> samples;
    rfmodel::ConversionDevice snapshot;
    rfmodel::ConversionResult converted;
    double worst_s_absolute_error;
    double worst_johnson_error_over_kt;
    double worst_equilibrium_flow_over_kt;
};

// Runs the example at 1, 2 and 3 MHz, checking independent V/I and Johnson
// oracles. The demonstration requires T>0 for its normalized error report.
Result run(Circuit circuit = {});
void verify_failure_and_ownership_cases();
} // namespace mna_noise_example
