#include "rfmodel/conversion_network.hpp"
#include "rfmodel/conversion_power_wave.hpp"
#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace rfmodel;

namespace {
using Vector = std::vector<Complex>;
constexpr std::size_t count = 7;
constexpr double spacing = 1e8;
constexpr double phase = .4;
constexpr double temperature = 290.;
constexpr double kt = 1.380649e-23 * temperature;
constexpr double source_variance = 1e-20;
const Complex carrier{.01, .005};
const Vector reference_template{
    {31., 12.}, {73., -21.}, {19., 8.}, {110., 31.}, {43., -12.}, {22., 7.}, {91., -18.}};
std::size_t wave_checks = 0, noise_checks = 0;
double maximum_wave_error = 0., maximum_noise_error = 0.;

// Native successor to the independent nominal_waves/change_waves/real_noise_bases
// oracle in tests/test_conversion_power_wave_file.py. Expected values below never
// call an RFModel solver, mixer, reference transform, or noise propagation helper.
// The strict tolerance is the original Python oracle's 4e-12 * max(scale, |a|, |b|).
void close(Complex actual, Complex expected, const std::string &where, double scale = 1.) {
    const double error = std::abs(actual - expected);
    const double tolerance = 4e-12 * std::max({scale, std::abs(actual), std::abs(expected)});
    if (!std::isfinite(error) || error > tolerance) {
        std::ostringstream message;
        message << std::setprecision(17) << where << ": got " << actual << ", expected " << expected
                << ", error " << error << ", tolerance " << tolerance;
        throw std::runtime_error(message.str());
    }
    if (scale < 1.) {
        ++noise_checks;
        maximum_noise_error = std::max(maximum_noise_error, error);
    } else {
        ++wave_checks;
        maximum_wave_error = std::max(maximum_wave_error, error);
    }
}

std::string at(const char *field, std::size_t i, std::size_t j) {
    return std::string(field) + "[" + std::to_string(i) + "," + std::to_string(j) + "]";
}

SMatrix zero_matrix(std::size_t n) {
    return {n, Vector(n * n)};
}

ConversionNoise zero_noise(std::size_t n) {
    return {zero_matrix(n), zero_matrix(n)};
}

void add_real_basis(ConversionNoise &noise, const Vector &basis) {
    require(noise.covariance.ports == basis.size(), "real noise basis dimensions");
    for (std::size_t i = 0; i < basis.size(); ++i) {
        for (std::size_t j = 0; j < basis.size(); ++j) {
            noise.covariance(i, j) += basis[i] * std::conj(basis[j]);
            noise.complementary(i, j) += basis[i] * basis[j];
        }
    }
}

Vector slice(const Vector &values, std::size_t begin, std::size_t end) {
    return Vector(values.begin() + begin, values.begin() + end);
}

struct Waves {
    Vector a, b;
};

// 2*x(t)*cos(10*w*t+phase), with real x(t) at bin 8, gives the RMS
// positive-frequency coefficients L=exp(i*phase)*conj(s), U=exp(i*phase)*s.
// V1-V2=25*I1 and I1=-I2 at common 50-ohm references give r=25/125=.2,
// t=100/125=.8. The mixer has no reverse conversion, hence no feedback solve
// is needed. source[0,4,6] are external 50-ohm incident waves. offset adds
// to device outgoing waves before wires are connected.
Waves nominal_waves(const Vector &source = Vector(count), const Vector &offset = Vector(count)) {
    const Complex pump = std::polar(1., phase);
    const Complex lower = pump * std::conj(source[0]) + offset[1];
    const Complex upper = pump * source[0] + offset[2];
    const Complex back_lower = .2 * lower + .8 * source[4] + offset[3];
    const Complex back_upper = .2 * upper + .8 * source[6] + offset[5];
    return {{source[0], back_lower, back_upper, lower, source[4], upper, source[6]},
            {offset[0],
             lower,
             upper,
             back_lower,
             .8 * lower + .2 * source[4] + offset[4],
             back_upper,
             .8 * upper + .2 * source[6] + offset[6]}};
}

// Kurokawa power-wave definitions, applied independently through physical V/I:
// a=(V+Z*I)/(2*sqrt(Re Z)), b=(V-conj(Z)*I)/(2*sqrt(Re Z)).
Waves change_waves(const Waves &common, const Vector &references) {
    Waves result{Vector(count), Vector(count)};
    for (std::size_t i = 0; i < count; ++i) {
        const Complex voltage = std::sqrt(50.) * (common.a[i] + common.b[i]);
        const Complex current = (common.a[i] - common.b[i]) / std::sqrt(50.);
        const double denominator = 2. * std::sqrt(references[i].real());
        result.a[i] = (voltage + references[i] * current) / denominator;
        result.b[i] = (voltage - std::conj(references[i]) * current) / denominator;
    }
    return result;
}

// Physical Thevenin boundary V+50*I=2*sqrt(50)*source (I into the device).
// Its local source gain and reflection are independent of RFModel helpers.
Complex boundary_gain(Complex reference) {
    return 2. * std::sqrt(50. * reference.real()) / (50. + std::conj(reference));
}

Complex boundary_reflection(Complex reference) {
    return (50. - reference) / (50. + std::conj(reference));
}

std::vector<Waves> real_noise_bases() {
    std::vector<Waves> result;
    // A circular complex source of variance v is sqrt(v/2)*(X+iY), with
    // independent real unit-variance X,Y. The series resistor's open-circuit
    // Johnson voltage has PSD 4*k*T*R; each 50-ohm outgoing noise wave is
    // +/-sqrt(50)*e/(25+2*50), giving variance .32*k*T. The signs enforce
    // perfect anti-correlation within each resistor frequency block.
    for (const Complex component : {Complex{1., 0.}, Complex{0., 1.}}) {
        Vector source(count);
        source[0] = component * std::sqrt(source_variance / 2.);
        result.push_back(nominal_waves(source));
        for (const std::size_t input : {std::size_t{3}, std::size_t{5}}) {
            Vector offset(count);
            offset[input] = component * std::sqrt(.32 * kt / 2.);
            offset[input + 1] = -offset[input];
            result.push_back(nominal_waves(Vector(count), offset));
        }
    }
    return result;
}

struct Fixture {
    FrequencyConversionNetwork network;
    Vector references, offset;
};

Fixture make_fixture(double scale, const Vector &source, const Vector &offset) {
    Vector references = reference_template;
    for (auto &reference : references) {
        reference *= scale;
    }
    const Vector mixer_references = slice(references, 0, 3);
    const Vector resistor_references = slice(references, 3, 7);
    const auto mixer = ideal_mixer_conversion(spacing, {{0, 8}, {1, 2}, {1, 18}}, 10, 0., phase);
    auto mixer_noise = mixer.zero_noise();
    mixer_noise.covariance(0, 0) = source_variance;
    const auto mixer_change = renormalize_power_waves(mixer, mixer_references);
    const auto mixer_boundary = renormalize_conversion_boundary(
        mixer, mixer_references, {0}, slice(source, 0, 3), Vector(3), mixer_noise);

    const IdealRLCModel resistor(
        "thermal25ohm", IdealElement::Resistor, LumpedConnection::SeriesImpedance, 25.);
    const auto lower_s = resistor.s_parameters(2 * spacing);
    const auto upper_s = resistor.s_parameters(18 * spacing);
    const auto linear = lift_linear_conversion(
        spacing,
        {2, 18},
        {lower_s, upper_s},
        {passive_thermal_noise(lower_s, temperature), passive_thermal_noise(upper_s, temperature)});
    const auto resistor_change = renormalize_power_waves(linear.model, resistor_references);
    const auto resistor_boundary = renormalize_conversion_boundary(linear.model,
                                                                   resistor_references,
                                                                   {1, 3},
                                                                   slice(source, 3, 7),
                                                                   Vector(4),
                                                                   linear.source_noise);

    // The network's legacy construction reference is deliberately unrelated
    // to the models' explicit per-channel references.
    FrequencyConversionNetwork network(spacing, 75.);
    network.add_power_waves({mixer_change.model,
                             mixer_boundary.source,
                             mixer_boundary.reflection,
                             mixer_boundary.source_noise,
                             mixer.zero_noise()});
    network.add_power_waves(
        {resistor_change.model,
         resistor_boundary.source,
         resistor_boundary.reflection,
         resistor_boundary.source_noise,
         renormalize_conversion_noise(resistor_change, linear.intrinsic_noise)});
    network.connect(0, 1, 1, 0);

    require(network.channel_count() == count, "seven-channel network dimensions");
    require(network.channel_offset(0) == 0 && network.channel_offset(1) == 3,
            "device-major channel offsets");
    require(network.reference_impedances_ohms() == references, "channel reference order");
    for (std::size_t i = 0; i < linear.model.channels().size(); ++i) {
        require(linear.model.channels()[i].port == i % 2 &&
                    linear.model.channels()[i].bin == (i < 2 ? 2 : 18),
                "linear lift must use frequency-major channel order");
    }
    for (const std::size_t i : {std::size_t{0}, std::size_t{4}, std::size_t{6}}) {
        const auto &boundary = i == 0 ? mixer_boundary : resistor_boundary;
        const auto local = i == 0 ? i : i - 3;
        close(boundary.source[local],
              boundary_gain(references[i]) * source[i],
              "physical boundary source");
        close(boundary.reflection[local],
              boundary_reflection(references[i]),
              "physical boundary reflection");
    }
    close(mixer_boundary.source_noise.covariance(0, 0),
          std::norm(boundary_gain(references[0])) * source_variance,
          "physical boundary source noise",
          source_variance);

    Vector local_offset = renormalize_conversion_offset(mixer_change, slice(offset, 0, 3));
    const auto resistor_offset =
        renormalize_conversion_offset(resistor_change, slice(offset, 3, 7));
    local_offset.insert(local_offset.end(), resistor_offset.begin(), resistor_offset.end());
    return {std::move(network), std::move(references), std::move(local_offset)};
}

void check_waves(const ConversionResult &actual,
                 const Waves &common,
                 const Vector &references,
                 bool passive_resistor) {
    const auto expected = change_waves(common, references);
    require(actual.incident.size() == count && actual.outgoing.size() == count,
            "result wave dimensions");
    Vector voltage(count), current(count);
    for (std::size_t i = 0; i < count; ++i) {
        close(actual.incident[i], expected.a[i], at("incident", i, 0));
        close(actual.outgoing[i], expected.b[i], at("outgoing", i, 0));
        voltage[i] =
            (std::conj(references[i]) * actual.incident[i] + references[i] * actual.outgoing[i]) /
            std::sqrt(references[i].real());
        current[i] = (actual.incident[i] - actual.outgoing[i]) / std::sqrt(references[i].real());
        close(
            voltage[i], std::sqrt(50.) * (common.a[i] + common.b[i]), at("physical voltage", i, 0));
        close(
            current[i], (common.a[i] - common.b[i]) / std::sqrt(50.), at("physical current", i, 0));
        const double net_power = std::norm(actual.incident[i]) - std::norm(actual.outgoing[i]);
        close(net_power,
              std::norm(common.a[i]) - std::norm(common.b[i]),
              at("net signal power", i, 0));
        close(net_power, (voltage[i] * std::conj(current[i])).real(), at("VI signal power", i, 0));
    }
    for (const auto &wire : {std::pair<std::size_t, std::size_t>{1, 3}, {2, 5}}) {
        close(voltage[wire.first], voltage[wire.second], "wire voltage continuity");
        close(current[wire.first], -current[wire.second], "wire current continuity");
    }
    if (passive_resistor) {
        for (const std::size_t i : {std::size_t{3}, std::size_t{5}}) {
            close(voltage[i] - voltage[i + 1], 25. * current[i], "resistor Ohm's law");
            close(current[i], -current[i + 1], "resistor current continuity");
            const double dissipated =
                std::norm(actual.incident[i]) + std::norm(actual.incident[i + 1]) -
                std::norm(actual.outgoing[i]) - std::norm(actual.outgoing[i + 1]);
            close(dissipated, 25. * std::norm(current[i]), "resistor Joule power");
        }
    }
    require(std::isfinite(actual.relative_residual) && actual.relative_residual < 1e-12,
            "network relative residual");
}

void check_noise(const ConversionResult &actual,
                 const std::vector<Waves> &common_bases,
                 const Vector &references) {
    std::vector<Waves> local_bases;
    for (const auto &basis : common_bases) {
        local_bases.push_back(change_waves(basis, references));
    }
    for (const auto *noise :
         {&actual.outgoing_noise, &actual.incident_noise, &actual.incident_outgoing_noise}) {
        require(noise->covariance.ports == count && noise->complementary.ports == count &&
                    noise->covariance.values.size() == count * count &&
                    noise->complementary.values.size() == count * count,
                "full loaded C/P matrix dimensions");
    }
    require(actual.net_noise_into_device_w_per_hz.size() == count, "net-noise dimensions");
    // Each basis is the response to one independent real, unit-variance random
    // variable. Outer products give C=E[x*y^H], P=E[x*y^T] directly. In
    // particular, cross matrices use incident first; neither is assumed to
    // be Hermitian/symmetric. Check every entry, including expected zeros.
    for (std::size_t i = 0; i < count; ++i) {
        double net_noise = 0.;
        for (const auto &basis : local_bases) {
            net_noise += std::norm(basis.a[i]) - std::norm(basis.b[i]);
        }
        close(actual.net_noise_into_device_w_per_hz[i],
              net_noise,
              at("net noise into device", i, 0),
              source_variance);
        for (std::size_t j = 0; j < count; ++j) {
            Complex caa{}, paa{}, cbb{}, pbb{}, cab{}, pab{};
            for (const auto &basis : local_bases) {
                caa += basis.a[i] * std::conj(basis.a[j]);
                paa += basis.a[i] * basis.a[j];
                cbb += basis.b[i] * std::conj(basis.b[j]);
                pbb += basis.b[i] * basis.b[j];
                cab += basis.a[i] * std::conj(basis.b[j]);
                pab += basis.a[i] * basis.b[j];
            }
            close(actual.incident_noise.covariance(i, j), caa, at("Caa", i, j), source_variance);
            close(actual.incident_noise.complementary(i, j), paa, at("Paa", i, j), source_variance);
            close(actual.outgoing_noise.covariance(i, j), cbb, at("Cbb", i, j), source_variance);
            close(actual.outgoing_noise.complementary(i, j), pbb, at("Pbb", i, j), source_variance);
            close(actual.incident_outgoing_noise.covariance(i, j),
                  cab,
                  at("Cab", i, j),
                  source_variance);
            close(actual.incident_outgoing_noise.complementary(i, j),
                  pab,
                  at("Pab", i, j),
                  source_variance);
        }
    }
}

void test_mixer_resistor(double scale) {
    Vector source(count);
    source[0] = carrier;
    const auto fixture = make_fixture(scale, source, Vector(count));
    const auto result = fixture.network.analyze(true);
    check_waves(result, nominal_waves(source), fixture.references, true);
    check_noise(result, real_noise_bases(), fixture.references);
    for (const std::size_t output : {std::size_t{4}, std::size_t{6}}) {
        const auto nf = fixture.network.reference_noise_analysis({0}, {0}, output, temperature);
        // Available-power gain |t|^2=.64; output thermal noise is
        // (.64+.32)*k*T, so F=1.5 and equivalent input temperature=145 K.
        close(nf.reference_gain, .64, "reference gain");
        close(nf.reference_output_noise_w_per_hz, .64 * kt, "reference noise", kt);
        close(nf.output_noise_w_per_hz, .96 * kt, "output thermal noise", kt);
        close(nf.noise_factor, 1.5, "noise factor");
        close(nf.noise_figure_db, 10. * std::log10(1.5), "noise figure");
        close(nf.equivalent_input_temperature_k, 145., "equivalent input temperature");
    }
}

void test_external_sources_offsets_and_correlated_noise(double scale) {
    Vector source(count);
    source[0] = carrier;
    source[4] = {-.007, .003};
    source[6] = {.006, -.009};
    const Vector offset{{.002, -.001},
                        {-.003, .002},
                        {.001, .004},
                        {.002, .003},
                        {-.001, .002},
                        {.004, -.002},
                        {-.003, -.001}};
    const auto fixture = make_fixture(scale, source, offset);
    auto additional = zero_noise(count);
    auto bases = real_noise_bases();
    // Independent real variables shared by all three external channels create
    // cross-device C and nonzero P, including RF-to-load and load-to-load terms.
    // Unlike the circular thermal sources these are intentionally improper.
    for (const auto &external : std::array<std::array<Complex, 3>, 2>{
             {{{{.4, .2}, {-.1, .3}, {.25, -.15}}}, {{{-.2, .35}, {.3, .1}, {-.05, -.2}}}}}) {
        Vector common(count), local(count);
        for (std::size_t k = 0; k < 3; ++k) {
            const auto i = std::array<std::size_t, 3>{{0, 4, 6}}[k];
            common[i] = std::sqrt(source_variance) * external[k];
            local[i] = boundary_gain(fixture.references[i]) * common[i];
        }
        add_real_basis(additional, local);
        bases.push_back(nominal_waves(common));
    }
    const auto result = fixture.network.analyze(true, &additional, fixture.offset);
    check_waves(result, nominal_waves(source, offset), fixture.references, false);
    check_noise(result, bases, fixture.references);
    // Affine output offsets and deterministic sources do not alter covariance.
    // Check the correlated-only path separately so an offset cannot hide errors.
    const auto without_offset = fixture.network.analyze(true, &additional);
    check_waves(without_offset, nominal_waves(source), fixture.references, true);
    check_noise(without_offset, bases, fixture.references);
}
} // namespace

int main() {
    try {
        for (const double scale : {.5, 1., 2.}) {
            test_mixer_resistor(scale);
            test_external_sources_offsets_and_correlated_noise(scale);
        }
        std::cout << std::setprecision(17)
                  << "conversion_mixer_physics_test: scales=0.5,1,2; wave_checks=" << wave_checks
                  << "; noise_checks=" << noise_checks
                  << "; maximum_wave_error=" << maximum_wave_error
                  << "; maximum_noise_error=" << maximum_noise_error << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "conversion_mixer_physics_test: " << error.what() << '\n';
        return 1;
    }
}
