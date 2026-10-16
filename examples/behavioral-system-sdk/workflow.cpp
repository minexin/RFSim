#include "workflow.hpp"
#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "rfmodel/conversion/conversion_simulation.hpp"
#include "rfmodel/linear/linear_analysis.hpp"
#include "rfmodel/linear/linear_path.hpp"
#include <iomanip>
#include <ostream>
#include <optional>

namespace behavioral_example {
namespace {
using namespace rfmodel;
constexpr double available_w = .001;
constexpr double reference = 50.;
void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(what); // Active under NDEBUG.
}
void close(Complex actual, Complex expected, const char *what) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()) &&
            std::abs(actual - expected) <= 2e-11 * std::max(1., std::abs(expected)), what);
}
struct Endpoint { std::size_t device, port; };
struct Fixture {
    std::vector<ModelAdapter> models;
    std::vector<ConversionPortConnection> wires;
    std::vector<Endpoint> external;
};
std::size_t global_port(const Fixture &f, Endpoint endpoint) {
    std::size_t offset = 0;
    for (std::size_t d = 0; d < endpoint.device; ++d)
        offset += f.models.at(d).descriptor().ports.size();
    require(endpoint.port < f.models.at(endpoint.device).descriptor().ports.size(), "port identity");
    return offset + endpoint.port;
}
std::vector<std::size_t> external_ports(const Fixture &f) {
    std::vector<std::size_t> result;
    for (auto endpoint : f.external) result.push_back(global_port(f, endpoint));
    return result;
}
LinearNetwork build(const Fixture &f, double frequency) {
    LinearNetwork network;
    for (const auto &model : f.models) {
        require(model.capabilities().linear, "example requires stationary linear capability");
        model.add_to(network, frequency);
    }
    for (auto wire : f.wires)
        network.connect(global_port(f, {wire.first_device, wire.first_port}),
                        global_port(f, {wire.second_device, wire.second_port}));
    return network;
}
ModelAdapter model(std::string name, SMatrix s, const std::vector<Complex> &refs) {
    const auto native = renormalize_power_waves(s, std::vector<Complex>(s.ports, reference), refs).scattering;
    std::vector<PortInfo> ports;
    for (std::size_t p = 0; p < refs.size(); ++p)
        ports.push_back({p, "p" + std::to_string(p), refs[p]});
    // Provider owns the model; no local-variable reference captures.
    return adapt_device_model(StaticSParameterModel(name, ports, native), {"example.static_s", "1", "", ""});
}
ModelAdapter resistor(std::string name, double resistance, std::vector<Complex> refs = {50., 50.}) {
    const double reflection = resistance / (100. + resistance);
    const double transmission = 100. / (100. + resistance);
    return model(std::move(name), {2, {reflection, transmission, transmission, reflection}}, refs);
}
Fixture chain() {
    return {{resistor("R1", 50), resistor("R2", 50)}, {{0, 1, 1, 0}}, {{0, 0}, {1, 1}}};
}
Fixture branch(bool mixed) {
    const std::vector<Complex> tee_refs = mixed ? std::vector<Complex>{50., {40., 10.}, {75., -5.}} : std::vector<Complex>(3, 50.);
    return {{model("tee", {3, {-1./3., 2./3., 2./3., 2./3., -1./3., 2./3., 2./3., 2./3., -1./3.}}, tee_refs),
             resistor("main", 50., mixed ? std::vector<Complex>{{60., 15.}, 50.} : std::vector<Complex>(2, 50.)),
             resistor("side", 50., mixed ? std::vector<Complex>{{35., -20.}, 50.} : std::vector<Complex>(2, 50.))},
            {{0, 1, 1, 0}, {0, 2, 2, 0}}, {{0, 0}, {1, 1}, {2, 1}}};
}
ConversionSimulationRequest request(const Fixture &f, double frequency, const std::vector<Complex> &loads) {
    require(loads.size() + 1 == f.external.size(), "load count");
    ConversionSimulationRequest req;
    for (const auto &m : f.models)
        req.devices.push_back(m.conversion_device(frequency, {1}, ModelNoisePolicy::omit_intrinsic_noise));
    req.connections = f.wires;
    const auto input = f.external.front();
    req.devices[input.device].source[input.port] = std::sqrt(available_w);
    for (std::size_t k = 1; k < f.external.size(); ++k) {
        auto ep = f.external[k];
        // External references are real 50 ohm in these examples.
        req.devices[ep.device].reflection[ep.port] = (loads[k-1] - reference) / (loads[k-1] + reference);
    }
    return req;
}
std::optional<Complex> loaded_apparent_ratio(const ConversionChannelState &channel) {
    if (std::abs(channel.current_a) <= 1e-15) return std::nullopt;
    return channel.voltage_v / channel.current_a;
}
void check_port(const ConversionSimulationResult &r, std::size_t p, Complex v, Complex i) {
    const auto &c = r.channels.at(p);
    close(c.voltage_v, v, "analytic RMS voltage");
    close(c.current_a, i, "analytic current entering device");
    close(c.net_into_device_w, (v * std::conj(i)).real(), "analytic net absorbed power");
    close(std::norm(r.waves.incident.at(p)) - std::norm(r.waves.outgoing.at(p)),
          c.net_into_device_w, "power-wave net power");
}
void report(std::ostream &out, const Fixture &f, double frequency, const ConversionSimulationResult &r) {
    for (std::size_t k = 0; k < r.channels.size(); ++k) {
        const auto &c = r.channels[k];
        const auto &d = f.models.at(c.channel.device).descriptor();
        out << d.name << '/' << d.ports.at(c.channel.port).name << " frequency_Hz=" << frequency
            << " bin=" << c.channel.bin << " reference_ohm=" << c.reference_ohms
            << " a_sqrtW=" << r.waves.incident[k] << " b_sqrtW=" << r.waves.outgoing[k]
            << " V_RMS=" << c.voltage_v << " I_into_device_RMS=" << c.current_a
            << " incident_W=" << std::norm(r.waves.incident[k])
            << " outgoing_W=" << std::norm(r.waves.outgoing[k])
            << " net_into_device_W=" << c.net_into_device_w;
        auto ratio = loaded_apparent_ratio(c);
        if (ratio) out << " loaded_apparent_V_over_I_ohm=" << *ratio;
        else out << " loaded_apparent_V_over_I_ohm=not_reported_below_current_floor_1e-15_A";
        out << '\n';
    }
}
void verify_linear(const Fixture &f, bool branched, std::ostream &out) {
    const auto sweep = analyze_linear({{1e9, 2e9}}, external_ports(f), [&](double hz) { return build(f, hz); });
    require(sweep.scattering.size() == 2 && sweep.frequencies_hz.size() == 2, "sweep cardinality");
    // Closed-form circuit S, obtained with each unexcited external port terminated
    // in 50 ohm. No production solver is used as the independent oracle.
    const double expected_chain[2][2] = {{.5, .5}, {.5, .5}};
    const double expected_branch[3][3] = {{0., .5, .5}, {.5, .25, .25}, {.5, .25, .25}};
    for (std::size_t k = 0; k < sweep.scattering.size(); ++k) {
        const auto &s = sweep.scattering[k];
        require(s.ports == f.external.size(), "external S port count");
        for (std::size_t i = 0; i < s.ports; ++i)
            for (std::size_t j = 0; j < s.ports; ++j) {
                close(s(i,j), branched ? expected_branch[i][j] : expected_chain[i][j], "analytic external S");
                out << "external_S " << i << ',' << j << " frequency_Hz=" << sweep.frequencies_hz[k] << " value=" << s(i,j) << '\n';
            }
    }
}
void verify_chain(std::ostream &out) {
    auto f = chain();
    verify_linear(f, false, out);
    require(!f.models[0].capabilities().noise, "static S has no noise callback");
    bool missing_noise = false;
    try {
        (void)f.models[0].conversion_device(1e9, {1}, ModelNoisePolicy::require_model_noise);
    } catch (const ModelCapabilityUnavailable &) { missing_noise = true; }
    require(missing_noise, "missing noise must be explicit, never silently zero");
    for (double frequency : {1e9, 2e9}) for (double load : {50., 100.}) {
        auto req = request(f, frequency, {load});
        const auto r = analyze_conversion_simulation(req);
        require(r.channels.size() == 4, "chain channel count");
        const double i = 2 * std::sqrt(reference * available_w) / (150 + load);
        check_port(r, 0, i * (100 + load), i);
        check_port(r, 1, i * (50 + load), -i);
        check_port(r, 2, i * (50 + load), i);
        check_port(r, 3, i * load, -i);
        // Optional chain-only convenience API: derive stages from the same models.
        std::vector<LinearPathStage> stages;
        for (const auto &m : f.models) stages.push_back({m.descriptor().name, m.s_parameters(frequency), reference});
        auto path = analyze_linear_path(stages, available_w, {}, (load-reference)/(load+reference));
        close(path.load_delivered_w, i*i*load, "chain path analytic load power");
        out << "chain R1/p0 -> R2/p1 load_ohm=" << load << " load_delivered_over_source_available=" << *path.transducer_gain << '\n';
        report(out, f, frequency, r);
        // A malformed physical connection fails; correcting it is a clean retry.
        auto invalid = req;
        invalid.connections.push_back(req.connections.front());
        bool rejected = false;
        try { (void)analyze_conversion_simulation(invalid); } catch (const std::invalid_argument &) { rejected = true; }
        require(rejected, "duplicate wire must fail");
        close(analyze_conversion_simulation(req).channels[0].voltage_v, r.channels[0].voltage_v, "retry changed result");
        // All results and requests own sampled state. Later parameter/source changes
        // cannot mutate an earlier result or a previously sampled request.
        auto changed = f;
        changed.models[0] = resistor("R1", 100.);
        auto next = analyze_conversion_simulation(request(changed, frequency, {load}));
        const double next_i = 2 * std::sqrt(reference*available_w)/(200+load);
        check_port(next, 3, next_i*load, -next_i);
        close(r.channels[3].voltage_v, i*load, "parameter change contaminated old result");
        close(analyze_conversion_simulation(req).channels[3].voltage_v, i*load, "parameter change contaminated sampled request");
        req.devices[0].source[0] = 0.;
        const auto silent = analyze_conversion_simulation(req);
        require(!loaded_apparent_ratio(silent.channels[0]), "zero-current ratio must be undefined");
        close(r.channels[3].voltage_v, i*load, "source change contaminated snapshot");
    }
}
void verify_branch(std::ostream &out) {
    const auto real = branch(false), mixed = branch(true);
    verify_linear(mixed, true, out);
    for (double frequency : {1e9, 2e9}) for (Complex side_load : {Complex{50., 0.}, Complex{100., 30.}}) {
        const Complex main_load{100., -20.};
        const auto r = analyze_conversion_simulation(request(mixed, frequency, {main_load, side_load}));
        require(r.channels.size() == 7, "branch channel count");
        const auto real_result = analyze_conversion_simulation(request(real, frequency, {main_load, side_load}));
        const Complex parallel = 1. / (1./(50.+main_load) + 1./(50.+side_load));
        const Complex input_i = 2.*std::sqrt(reference*available_w)/(50.+parallel);
        const Complex junction_v = input_i*parallel;
        const Complex main_i = junction_v/(50.+main_load), side_i = junction_v/(50.+side_load);
        check_port(r, 0, junction_v, input_i);
        check_port(r, 1, junction_v, -main_i);
        check_port(r, 2, junction_v, -side_i);
        check_port(r, 3, junction_v, main_i);
        check_port(r, 4, main_i*main_load, -main_i);
        check_port(r, 5, junction_v, side_i);
        check_port(r, 6, side_i*side_load, -side_i);
        close(r.channels[0].current_a + r.channels[1].current_a + r.channels[2].current_a, 0., "solved junction KCL");
        for (const auto &wire : mixed.wires) {
            const auto &a = r.channels[global_port(mixed, {wire.first_device, wire.first_port})];
            const auto &b = r.channels[global_port(mixed, {wire.second_device, wire.second_port})];
            close(a.voltage_v, b.voltage_v, "wire voltage continuity");
            close(a.current_a, -b.current_a, "wire KCL");
        }
        double net = 0.;
        for (std::size_t k = 0; k < r.channels.size(); ++k) {
            close(r.channels[k].voltage_v, real_result.channels[k].voltage_v, "reference-coordinate voltage invariance");
            close(r.channels[k].current_a, real_result.channels[k].current_a, "reference-coordinate current invariance");
            net += r.channels[k].net_into_device_w;
        }
        close(net, 50.*(std::norm(main_i)+std::norm(side_i)), "total resistor dissipation");
        const double main_delivered = -r.channels[4].net_into_device_w;
        const double accepted = r.channels[0].net_into_device_w;
        close(main_delivered/accepted, std::norm(main_i)*main_load.real()/(junction_v*std::conj(input_i)).real(), "selected-cut ratio oracle");
        out << "selected_cuts tee/p0 (net into tee) -> main/p1 (net out to load), frequency_Hz=" << frequency
            << " source=50ohm_1mW_available main_load_ohm=" << main_load << " side_load_ohm=" << side_load
            << " main_delivered_over_tee_input_accepted=" << main_delivered/accepted << '\n';
        report(out, mixed, frequency, r);
    }
}
} // namespace
void verify_and_report(std::ostream &out) {
    out << std::setprecision(12);
    verify_chain(out);
    const auto detached = request(chain(), 1e9, {100.});
    const auto detached_result = analyze_conversion_simulation(detached);
    const double detached_i = 2.*std::sqrt(reference*available_w)/250.;
    check_port(detached_result, 3, detached_i*100., -detached_i);
    verify_branch(out);
    out << "PASS: shared behavioral models/wiring; external S, loaded named ports, chain and full-graph selected cuts; analytic oracles; snapshots/retry/zero current\n";
}
} // namespace behavioral_example
