#include "fixture.hpp"
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>

namespace {
using rfmodel::Complex;
void number(std::ostream &out, double value) {
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite JSON number");
    out << value;
}
void complex(std::ostream &out, Complex value) {
    out << "{\"real\":"; number(out, value.real());
    out << ",\"imag\":"; number(out, value.imag()); out << '}';
}
void emit(std::ostream &out) {
    using namespace res_probe;
    const auto f = fixture();
    const auto linear = rfmodel::analyze_linear({{frequency_hz}}, f.external_ports,
        [&](double hz) { return linear_network(f, hz); });
    const auto loaded = rfmodel::analyze_conversion_simulation(loaded_request(f));
    std::vector<rfmodel::LinearPathStage> stages;
    for (const auto &model : f.models)
        stages.push_back({model.descriptor().name, model.s_parameters(frequency_hz), reference_ohms});
    const auto path = rfmodel::analyze_linear_path(stages, source_available_w);
    if (linear.scattering.size() != 1 || linear.scattering[0].ports != 2 ||
        loaded.channels.size() != 4 || !path.transducer_gain)
        throw std::runtime_error("unexpected native result shape");
    out << "{\n\"schema_version\":1,\n"
        "\"plan_id\":\"systemvue-linear-rfsystem-minimal-20261016-v1\",\n"
        "\"provenance_class\":\"RFModel_installed_SDK_native_result_not_vendor_data\",\n"
        "\"fixture\":{\"frequency_hz\":100000000,\"source_available_w\":0.001,"
        "\"source_phase_deg\":0,\"source_ohms\":50,\"load_ohms\":50,"
        "\"resistors\":[{\"name\":\"R1\",\"ohms\":50},{\"name\":\"R2\",\"ohms\":50}],"
        "\"ordered_nodes\":[\"IN\",\"MID\",\"OUT\"],"
        "\"connection\":{\"first\":\"R1/p1\",\"second\":\"R2/p0\",\"node\":\"MID\"},"
        "\"noise_policy\":\"omit_intrinsic_noise\",\"noise_validation_performed\":false,"
        "\"deterministic_single_cw\":true,\"vendor_channel_integration_performed\":false},\n"
        "\"conventions\":{\"complex_encoding\":\"binary64_roundtrip_real_imag\","
        "\"voltage\":\"RMS_phasor_to_common_ground_V\","
        "\"current\":\"RMS_phasor_entering_named_device_port_A\","
        "\"wave\":\"power_wave_sqrt_W_incident_into_device_outgoing_from_device\","
        "\"net_power\":\"positive_into_named_device_port_W\","
        "\"phase_reference\":\"real_positive_source_incident_wave\","
        "\"peak_voltage\":\"sqrt_2_times_abs_loaded_RMS_phasor_single_CW_only\","
        "\"vendor_measurement_aliases\":false},\n"
        "\"linear\":{\"frequency_hz\":";
    number(out, linear.frequencies_hz.at(0));
    out << ",\"port_order\":[\"IN\",\"OUT\"],\"reference_ohms\":[";
    complex(out, linear.port_impedances_ohms.at(0).at(0)); out << ',';
    complex(out, linear.port_impedances_ohms.at(0).at(1));
    out << "],\"S_order\":\"row_response_column_excitation\",\"S_shape\":[2,2],\"S_row_major\":[";
    for (std::size_t row = 0; row < 2; ++row) for (std::size_t col = 0; col < 2; ++col) {
        if (row || col) out << ',';
        complex(out, linear.scattering.at(0)(row, col));
    }
    out << "],\"noise_requested\":false},\n\"loaded_channels\":[\n";
    const char *nodes[] = {"IN", "MID", "MID", "OUT"};
    for (std::size_t i = 0; i < loaded.channels.size(); ++i) {
        const auto &c = loaded.channels[i];
        if (c.channel.device != i / 2 || c.channel.port != i % 2 || c.channel.bin != 1)
            throw std::runtime_error("unexpected native channel order");
        if (i) out << ",\n";
        out << "{\"device\":\"" << f.models.at(c.channel.device).descriptor().name
            << "\",\"device_index\":" << c.channel.device << ",\"port\":\"p" << c.channel.port
            << "\",\"port_index\":" << c.channel.port << ",\"node\":\"" << nodes[i]
            << "\",\"bin\":1,\"frequency_hz\":";
        number(out, frequency_hz); out << ",\"reference_ohms\":"; complex(out, c.reference_ohms);
        out << ",\"voltage_rms_v\":"; complex(out, c.voltage_v);
        out << ",\"current_into_device_rms_a\":"; complex(out, c.current_a);
        out << ",\"incident_wave_sqrt_w\":"; complex(out, loaded.waves.incident.at(i));
        out << ",\"outgoing_wave_sqrt_w\":"; complex(out, loaded.waves.outgoing.at(i));
        out << ",\"incident_wave_power_w\":"; number(out, std::norm(loaded.waves.incident.at(i)));
        out << ",\"outgoing_wave_power_w\":"; number(out, std::norm(loaded.waves.outgoing.at(i)));
        out << ",\"net_into_device_w\":"; number(out, c.net_into_device_w);
        out << ",\"loaded_single_cw_peak_voltage_v\":"; number(out, std::sqrt(2.) * std::abs(c.voltage_v));
        out << ",\"internally_connected\":" << (c.connected ? "true" : "false") << '}';
    }
    out << "\n],\n\"path_power\":{\"source_available_w\":"; number(out, path.source_available_w);
    out << ",\"dut_input_accepted_w\":"; number(out, path.input_accepted_w);
    out << ",\"mid_delivered_to_R2_and_load_w\":"; number(out, path.stages.at(1).input.absorbed_w);
    out << ",\"load_delivered_w\":"; number(out, path.load_delivered_w);
    out << ",\"load_delivered_dbm\":"; number(out, 10. * std::log10(path.load_delivered_w / .001));
    out << ",\"load_over_source_available_ratio\":"; number(out, *path.transducer_gain);
    out << ",\"load_over_dut_input_accepted_ratio\":"; number(out, path.load_delivered_w / path.input_accepted_w);
    out << ",\"R1_dissipated_w\":";
    number(out, path.stages.at(0).input.absorbed_w + path.stages.at(0).output.absorbed_w);
    out << ",\"R2_dissipated_w\":";
    number(out, path.stages.at(1).input.absorbed_w + path.stages.at(1).output.absorbed_w);
    out << "}\n}\n";
}
} // namespace
int main(int argc, char **) {
    try {
        if (argc != 1) throw std::invalid_argument("usage: behavioral_res_probe (JSON to stdout)");
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10);
        emit(out); // Complete in memory: failures never emit a partial JSON document.
        std::cout << out.str();
        if (!std::cout) throw std::runtime_error("writing native JSON failed");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
