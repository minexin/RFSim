#include "mna_noise.hpp"
#include <rfmodel/mna/power_wave_s.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mna_noise_example {
namespace rf = rfmodel;
namespace mna = rfmodel::mna;
using rf::Complex;

namespace {
constexpr double boltzmann = 1.380649e-23;
constexpr double tolerance = 2e-11;
const std::array<double, 3> frequencies{{1e6, 2e6, 3e6}};

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(Complex actual, Complex expected, double scale, const char *message) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()) &&
                std::abs(actual - expected) <= tolerance * scale,
            message);
}

template <class Exception, class Function> void rejects(Function call, const char *message) {
    bool rejected = false;
    try {
        call();
    } catch (const Exception &) {
        rejected = true;
    }
    require(rejected, message);
}

std::vector<Complex> references(const Circuit &circuit) {
    return {circuit.references_ohm.begin(), circuit.references_ohm.end()};
}

// Independent physical elimination: V1-V2=Z*I, I1=I, I2=-I.
// With a=0, a resistor's series voltage noise e_n drives I=e_n/D;
// b=(-sqrt(Re z1), +sqrt(Re z2))*e_n/D. E|e_n|^2=4*k*T*R.
// This oracle uses neither MNA stamping nor I-S*S^H nor SDK matrix solvers.
struct Oracle {
    rf::SMatrix scattering;
    rf::NoiseCorrelation noise;
};

Oracle oracle(const Circuit &circuit, double hz) {
    const auto &z = circuit.references_ohm;
    const Complex denominator =
        z[0] + z[1] +
        Complex{circuit.resistance_ohm, 2. * std::acos(-1.) * hz * circuit.inductance_h};
    const double root = std::sqrt(z[0].real() * z[1].real());
    const double density =
        4. * boltzmann * circuit.temperature_k * circuit.resistance_ohm / std::norm(denominator);
    return {
        {2,
         {1. - 2. * z[0].real() / denominator,
          2. * root / denominator,
          2. * root / denominator,
          1. - 2. * z[1].real() / denominator}},
        {{2, {density * z[0].real(), -density * root, -density * root, density * z[1].real()}}}};
}

// Three independent physical Johnson voltage sources, before any wave algebra.
// The source and load resistor voltages are Es and El; the DUT resistor is Ed.
// J=(Es-El-Ed)/(Zs+Zl+R+j*w*L), V1=Es-Zs*J, V2=El+Zl*J.
struct LoadedOracle {
    rf::SMatrix incident{2, std::vector<Complex>(4)};
    rf::SMatrix outgoing{2, std::vector<Complex>(4)};
};

LoadedOracle loaded_oracle(const Circuit &circuit,
                           double hz,
                           const std::array<Complex, 2> &loads,
                           const std::array<double, 2> &boundary_temperatures) {
    LoadedOracle result;
    const auto &refs = circuit.references_ohm;
    const Complex denominator =
        loads[0] + loads[1] +
        Complex{circuit.resistance_ohm, 2. * std::acos(-1.) * hz * circuit.inductance_h};
    for (std::size_t source = 0; source < 3; ++source) {
        const Complex es = source == 0 ? 1. : 0.;
        const Complex el = source == 1 ? 1. : 0.;
        const Complex ed = source == 2 ? 1. : 0.;
        const Complex current = (es - el - ed) / denominator;
        const std::array<Complex, 2> voltage{{es - loads[0] * current, el + loads[1] * current}};
        const std::array<Complex, 2> entering{{current, -current}};
        std::array<Complex, 2> a{}, b{};
        const double density = 4. * boltzmann *
                               (source < 2 ? boundary_temperatures[source] * loads[source].real()
                                           : circuit.temperature_k * circuit.resistance_ohm);
        for (std::size_t p = 0; p < 2; ++p) {
            const double denominator_wave = 2. * std::sqrt(refs[p].real());
            a[p] = (voltage[p] + refs[p] * entering[p]) / denominator_wave;
            b[p] = (voltage[p] - std::conj(refs[p]) * entering[p]) / denominator_wave;
        }
        for (std::size_t p = 0; p < 2; ++p) {
            for (std::size_t q = 0; q < 2; ++q) {
                result.incident(p, q) += density * a[p] * std::conj(a[q]);
                result.outgoing(p, q) += density * b[p] * std::conj(b[q]);
            }
        }
    }
    return result;
}

// DUT boundary a=gamma*b+e; this is not the external load's own S.
Complex reflection(Complex load, Complex reference) {
    return (load - reference) / (load + std::conj(reference));
}

void check_matrix(const rf::SMatrix &actual,
                  const rf::SMatrix &expected,
                  double scale,
                  const char *message) {
    require(actual.ports == expected.ports && actual.values.size() == expected.values.size(),
            "matrix dimensions differ");
    for (std::size_t i = 0; i < actual.values.size(); ++i) {
        near(actual.values[i], expected.values[i], scale, message);
    }
}

void check_zero(const rf::SMatrix &matrix, const char *message) {
    for (auto value : matrix.values) {
        require(value == Complex{}, message);
    }
}
} // namespace

rf::ModelAdapter make_model(Circuit circuit, Noise noise) {
    if (!std::isfinite(circuit.resistance_ohm) || circuit.resistance_ohm <= 0. ||
        !std::isfinite(circuit.inductance_h) || circuit.inductance_h <= 0. ||
        !std::isfinite(circuit.temperature_k) || circuit.temperature_k < 0. ||
        (noise != Noise::thermal && noise != Noise::missing && noise != Noise::explicit_zero)) {
        throw std::invalid_argument("example requires finite positive R/L and nonnegative T");
    }
    mna::NetworkBuilder builder;
    const auto left = builder.add_node();
    const auto middle = builder.add_node();
    const auto right = builder.add_node();
    builder.add_resistor(left, middle, circuit.resistance_ohm);
    builder.add_inductor(middle, right, circuit.inductance_h);
    const auto network = builder.finalize();
    const std::vector<mna::Port> ports{{left, mna::ground}, {right, mna::ground}};
    const auto refs = references(circuit);
    const mna::SolveOptions options{};
    rf::ModelFunctions functions;
    functions.scattering = [network, ports, refs, options](double hz) {
        return mna::extract_power_wave_s(network, hz, ports, refs, options).scattering;
    };
    if (noise != Noise::missing) {
        // The thermal model is justified by this known passive circuit and its
        // explicit uniform temperature, never by deactivating an unknown source.
        const double temperature = noise == Noise::explicit_zero ? 0. : circuit.temperature_k;
        functions.noise = [response = functions.scattering, temperature](double hz) {
            return rf::passive_thermal_noise(response(hz), temperature);
        };
    }
    return {{{"example.mna-rl", "1", "", ""},
             "Source-free isothermal series RL",
             {{0, "left", refs[0]}, {1, "right", refs[1]}}},
            std::move(functions)};
}

Result run(Circuit circuit) {
    if (!std::isfinite(circuit.temperature_k) || circuit.temperature_k <= 0.) {
        throw std::invalid_argument("normalized example report requires T>0");
    }
    const auto model = make_model(circuit);
    const auto caps = model.capabilities();
    require(caps.linear && caps.noise && !caps.spectrum && !caps.operating_point && !caps.coherent,
            "incorrect adapter capabilities");
    const auto refs = references(circuit);
    const double kt = boltzmann * circuit.temperature_k;
    const std::array<Complex, 2> loads{{{6., 1.}, {12., -4.}}};
    const std::vector<Complex> gamma{reflection(loads[0], refs[0]), reflection(loads[1], refs[1])};
    std::vector<Sample> samples;
    double worst_s = 0., worst_c = 0., worst_flow = 0.;
    for (double hz : frequencies) {
        const auto expected = oracle(circuit, hz);
        rf::LinearNetwork single;
        const auto offset = model.add_to(single, hz);
        const auto s = single.external_s({offset, offset + 1});
        const auto c = single.external_noise({offset, offset + 1}, model.noise_correlation(hz));
        check_matrix(s, expected.scattering, 1., "series RL S oracle mismatch");
        check_matrix(c.watts_per_hz, expected.noise.watts_per_hz, kt, "Johnson 4kTR mismatch");
        require(single.port_reference_impedance_ohms(offset) == refs[0] &&
                    single.port_reference_impedance_ohms(offset + 1) == refs[1],
                "single block reference order changed");
        for (std::size_t i = 0; i < 4; ++i) {
            worst_s = std::max(worst_s, std::abs(s.values[i] - expected.scattering.values[i]));
            worst_c = std::max(
                worst_c,
                std::abs(c.watts_per_hz.values[i] - expected.noise.watts_per_hz.values[i]) / kt);
        }
        // Repeated reads of the const owning callbacks do not mutate their state.
        check_matrix(model.s_parameters(hz), s, 1., "repeat S changed");
        check_matrix(
            model.noise_correlation(hz).watts_per_hz, c.watts_per_hz, kt, "repeat C changed");

        const auto boundary =
            rf::thermal_boundary_noise(gamma, {circuit.temperature_k, circuit.temperature_k});
        const auto equilibrium = rf::loaded_noise(s, c, gamma, boundary);
        const auto thermal_expected =
            loaded_oracle(circuit, hz, loads, {circuit.temperature_k, circuit.temperature_k});
        check_matrix(equilibrium.incident.watts_per_hz,
                     thermal_expected.incident,
                     kt,
                     "loaded equilibrium incident C mismatch");
        check_matrix(equilibrium.outgoing.watts_per_hz,
                     thermal_expected.outgoing,
                     kt,
                     "loaded equilibrium outgoing C mismatch");
        for (double flow : equilibrium.net_into_device_w_per_hz) {
            near(flow, 0., kt, "nonzero equilibrium net thermal flow");
            worst_flow = std::max(worst_flow, std::abs(flow) / kt);
        }
        // Warm DUT, zero-temperature boundaries: resistor emission must escape.
        const auto cold =
            rf::loaded_noise(s, c, gamma, rf::thermal_boundary_noise(gamma, {0., 0.}));
        const auto cold_expected = loaded_oracle(circuit, hz, loads, {0., 0.});
        check_matrix(cold.incident.watts_per_hz,
                     cold_expected.incident,
                     kt,
                     "cold boundary incident C mismatch");
        check_matrix(cold.outgoing.watts_per_hz,
                     cold_expected.outgoing,
                     kt,
                     "cold boundary outgoing C mismatch");
        require(cold.net_into_device_w_per_hz[0] < 0. && cold.net_into_device_w_per_hz[1] < 0.,
                "cold boundaries did not absorb resistor emission");

        // Deterministic loaded V/I from the extracted S, checked against a
        // separate physical Thevenin series circuit. RMS units throughout.
        const Complex voltage{.2, .1};
        const Complex total =
            loads[0] + loads[1] +
            Complex{circuit.resistance_ohm, 2. * std::acos(-1.) * hz * circuit.inductance_h};
        const Complex current = voltage / total;
        single.terminate(offset,
                         gamma[0],
                         voltage * std::sqrt(refs[0].real()) / (loads[0] + std::conj(refs[0])));
        single.terminate(offset + 1, gamma[1]);
        const auto waves = single.solve();
        std::array<Complex, 2> actual_v{}, actual_i{};
        for (std::size_t p = 0; p < 2; ++p) {
            const auto a = waves.incident[offset + p], b = waves.outgoing[offset + p];
            actual_v[p] = (std::conj(refs[p]) * a + refs[p] * b) / std::sqrt(refs[p].real());
            actual_i[p] = (a - b) / std::sqrt(refs[p].real());
            near(actual_v[p],
                 p == 0 ? voltage - loads[0] * current : loads[1] * current,
                 std::abs(voltage),
                 "loaded voltage oracle mismatch");
            near(actual_i[p],
                 p == 0 ? current : -current,
                 std::abs(current),
                 "loaded current oracle mismatch");
        }
        double absorbed = 0.;
        for (std::size_t p = 0; p < 2; ++p) {
            absorbed +=
                std::norm(waves.incident[offset + p]) - std::norm(waves.outgoing[offset + p]);
        }
        near(absorbed,
             circuit.resistance_ohm * std::norm(current),
             circuit.resistance_ohm * std::norm(current),
             "RMS dissipated power mismatch");

        // Both S and C must move together. Re-extraction and a new independent
        // Johnson oracle check the transformed coordinates, not just labels.
        auto moved = circuit;
        moved.references_ohm = {{{50., 7.}, {30., -11.}}};
        const auto moved_refs = references(moved);
        const auto transform = rf::renormalize_power_waves(s, refs, moved_refs);
        const auto transformed_c = rf::renormalize_noise(s, c, refs, moved_refs);
        const auto moved_model = make_model(moved);
        const auto moved_expected = oracle(moved, hz);
        check_matrix(transform.scattering,
                     moved_model.s_parameters(hz),
                     1.,
                     "renormalized S differs from re-extraction");
        check_matrix(
            transform.scattering, moved_expected.scattering, 1., "renormalized S oracle mismatch");
        check_matrix(transformed_c.watts_per_hz,
                     moved_model.noise_correlation(hz).watts_per_hz,
                     kt,
                     "renormalized C differs from re-extraction");
        check_matrix(transformed_c.watts_per_hz,
                     moved_expected.noise.watts_per_hz,
                     kt,
                     "renormalized Johnson oracle mismatch");

        const std::vector<Complex> moved_gamma{reflection(loads[0], moved_refs[0]),
                                               reflection(loads[1], moved_refs[1])};
        const auto moved_cold = rf::loaded_noise(transform.scattering,
                                                 transformed_c,
                                                 moved_gamma,
                                                 rf::thermal_boundary_noise(moved_gamma, {0., 0.}));
        const auto moved_cold_expected = loaded_oracle(moved, hz, loads, {0., 0.});
        check_matrix(moved_cold.incident.watts_per_hz,
                     moved_cold_expected.incident,
                     kt,
                     "renormalized loaded incident C mismatch");
        check_matrix(moved_cold.outgoing.watts_per_hz,
                     moved_cold_expected.outgoing,
                     kt,
                     "renormalized loaded outgoing C mismatch");
        for (std::size_t p = 0; p < 2; ++p) {
            near(moved_cold.net_into_device_w_per_hz[p],
                 cold.net_into_device_w_per_hz[p],
                 kt,
                 "nonzero thermal flow changed with wave reference");
        }

        // Two independent isothermal RL blocks, deliberately different complex
        // internal reference coordinates. The 4x4 noise covers all global ports.
        auto second_circuit = circuit;
        second_circuit.references_ohm = {{{16., 1.}, {25., -4.}}};
        const auto second = make_model(second_circuit);
        rf::LinearNetwork cascade;
        const auto first_offset = model.add_to(cascade, hz);
        const auto second_offset = second.add_to(cascade, hz);
        cascade.connect(first_offset + 1, second_offset);
        const std::vector<std::size_t> external{first_offset, second_offset + 1};
        const std::vector<rf::NoiseCorrelation> blocks{c, second.noise_correlation(hz)};
        const auto cascade_s = cascade.external_s(external);
        const auto cascade_c = cascade.external_noise(external, rf::independent_noise(blocks));
        auto total_circuit = circuit;
        total_circuit.resistance_ohm *= 2.;
        total_circuit.inductance_h *= 2.;
        total_circuit.references_ohm[1] = second_circuit.references_ohm[1];
        const auto cascade_expected = oracle(total_circuit, hz);
        check_matrix(cascade_s, cascade_expected.scattering, 1., "two-block S oracle mismatch");
        check_matrix(cascade_c.watts_per_hz,
                     cascade_expected.noise.watts_per_hz,
                     kt,
                     "two-block Johnson mismatch");
        const auto contributions = cascade.external_noise_contributions(external, blocks);
        require(contributions.size() == 2, "missing independent noise contribution");
        for (const auto &contribution : contributions) {
            for (std::size_t i = 0; i < 4; ++i) {
                near(contribution.watts_per_hz.values[i],
                     .5 * cascade_expected.noise.watts_per_hz.values[i],
                     kt,
                     "individual resistor Johnson contribution mismatch");
            }
        }
        samples.push_back(
            {hz, s, c, equilibrium, actual_v[1], -actual_i[1], absorbed, cascade_s, cascade_c});
    }

    // The temporary adapter, builder and all captured input copies are destroyed
    // before this returned, owning six-channel snapshot is inspected or used.
    auto snapshot = [circuit] {
        return make_model(circuit).conversion_device(
            1e6, {1, 2, 3}, rf::ModelNoisePolicy::require_model_noise);
    }();
    require(snapshot.model.channels().size() == 6, "conversion channel count");
    for (std::size_t r = 0; r < 6; ++r) {
        require(snapshot.model.channels()[r].port == r % 2 &&
                    snapshot.model.channels()[r].bin == static_cast<int>(r / 2 + 1) &&
                    snapshot.model.reference_impedances_ohms()[r] == refs[r % 2],
                "conversion channel order or reference mismatch");
        const auto expected = oracle(circuit, frequencies[r / 2]);
        for (std::size_t c = 0; c < 6; ++c) {
            const bool same_frequency = r / 2 == c / 2;
            near(snapshot.model.direct()(r, c),
                 same_frequency ? expected.scattering(r % 2, c % 2) : Complex{},
                 1.,
                 "conversion direct matrix mismatch");
            near(snapshot.model.conjugate()(r, c), 0., 1., "unexpected conjugate conversion");
            near(snapshot.intrinsic_noise.covariance(r, c),
                 same_frequency ? expected.noise.watts_per_hz(r % 2, c % 2) : Complex{},
                 kt,
                 "full conversion C mismatch");
            near(snapshot.intrinsic_noise.complementary(r, c),
                 0.,
                 kt,
                 "positive-frequency stationary P must be zero");
        }
    }
    // Every bin is excited independently; no mixing is invented by this linear lift.
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        snapshot.source[2 * k] = {.2 + .03 * static_cast<double>(k), .1};
    }
    rf::FrequencyConversionNetwork conversion(1e6);
    conversion.add_power_waves(snapshot);
    auto converted = conversion.analyze();
    for (std::size_t r = 0; r < 6; ++r) {
        const auto expected = oracle(circuit, frequencies[r / 2]);
        near(converted.outgoing[r],
             expected.scattering(r % 2, 0) * snapshot.source[2 * (r / 2)],
             std::abs(snapshot.source[2 * (r / 2)]),
             "conversion output wave mismatch");
    }
    check_matrix(converted.outgoing_noise.covariance,
                 snapshot.intrinsic_noise.covariance,
                 kt,
                 "conversion lost full C");
    check_matrix(converted.outgoing_noise.complementary,
                 snapshot.intrinsic_noise.complementary,
                 kt,
                 "conversion changed P");
    return {std::move(samples),
            std::move(snapshot),
            std::move(converted),
            worst_s,
            worst_c,
            worst_flow};
}

void verify_failure_and_ownership_cases() {
    Circuit input;
    const auto saved = input;
    const auto model = make_model(input);
    input.resistance_ohm = 99.;
    input.references_ohm[0] = {51., -17.};
    check_matrix(model.s_parameters(1e6),
                 oracle(saved, 1e6).scattering,
                 1.,
                 "factory retained input reference");
    const auto kt = boltzmann * saved.temperature_k;
    const auto missing = make_model(saved, Noise::missing);
    const auto zero = make_model(saved, Noise::explicit_zero);
    require(!missing.capabilities().noise && zero.capabilities().noise,
            "missing noise equals zero");
    rejects<rf::ModelCapabilityUnavailable>(
        [&] {
            missing.noise_correlation(1e6);
        },
        "missing noise accepted");
    rejects<rf::ModelCapabilityUnavailable>(
        [&] {
            missing.conversion_device(1e6, {1, 2, 3}, rf::ModelNoisePolicy::require_model_noise);
        },
        "conversion accepted missing required noise");
    const auto omitted =
        missing.conversion_device(1e6, {1, 2, 3}, rf::ModelNoisePolicy::omit_intrinsic_noise);
    const auto explicit_zero =
        zero.conversion_device(1e6, {1, 2, 3}, rf::ModelNoisePolicy::require_model_noise);
    check_zero(omitted.intrinsic_noise.covariance, "omitted C is not zero");
    check_zero(omitted.intrinsic_noise.complementary, "omitted P is not zero");
    check_zero(explicit_zero.intrinsic_noise.covariance, "explicit zero C is not zero");
    check_zero(explicit_zero.intrinsic_noise.complementary, "explicit zero P is not zero");
    rejects<mna::SolveError>(
        [&] {
            model.s_parameters(0.);
        },
        "MNA accepted DC");
    rejects<mna::SolveError>(
        [&] {
            model.noise_correlation(0.);
        },
        "noise accepted MNA DC");
    for (double hz :
         {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        rejects<std::invalid_argument>(
            [&] {
                model.s_parameters(hz);
            },
            "invalid model frequency accepted");
    }
    for (const std::vector<int> &bins : {std::vector<int>{0, 1}, {1, 1}, {2, 1}, {-1, 1}}) {
        rejects<std::invalid_argument>(
            [&] {
                model.conversion_device(1e6, bins, rf::ModelNoisePolicy::require_model_noise);
            },
            "invalid conversion bins accepted");
    }
    for (double temperature :
         {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto bad = saved;
        bad.temperature_k = temperature;
        rejects<std::invalid_argument>(
            [&] {
                make_model(bad);
            },
            "invalid temperature accepted");
    }
    for (double value : {0.,
                         -1.,
                         std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
        auto bad_resistor = saved;
        bad_resistor.resistance_ohm = value;
        rejects<std::invalid_argument>(
            [&] {
                make_model(bad_resistor);
            },
            "invalid R accepted");
        auto bad_inductor = saved;
        bad_inductor.inductance_h = value;
        rejects<std::invalid_argument>(
            [&] {
                make_model(bad_inductor);
            },
            "invalid L accepted");
    }
    rejects<std::invalid_argument>(
        [&] {
            make_model(saved, static_cast<Noise>(99));
        },
        "invalid noise selection accepted");
    auto zero_temperature = saved;
    zero_temperature.temperature_k = 0.;
    const auto thermal_zero = make_model(zero_temperature);
    check_matrix(thermal_zero.s_parameters(1e6),
                 oracle(saved, 1e6).scattering,
                 1.,
                 "T=0 changed deterministic S");
    check_zero(thermal_zero.noise_correlation(1e6).watts_per_hz, "T=0 classical noise is not zero");
    const auto thermal_zero_snapshot =
        thermal_zero.conversion_device(1e6, {1, 2, 3}, rf::ModelNoisePolicy::require_model_noise);
    check_zero(thermal_zero_snapshot.intrinsic_noise.covariance, "thermal T=0 C is not zero");
    check_zero(thermal_zero_snapshot.intrinsic_noise.complementary, "thermal T=0 P is not zero");
    rejects<std::invalid_argument>(
        [&] {
            run(zero_temperature);
        },
        "normalized run accepted T=0");
    auto bad = saved;
    bad.references_ohm[0] = {0., 3.};
    rejects<std::invalid_argument>(
        [&] {
            make_model(bad);
        },
        "invalid reference accepted");
    bad.references_ohm[0] = {4., std::numeric_limits<double>::quiet_NaN()};
    rejects<std::invalid_argument>(
        [&] {
            make_model(bad);
        },
        "nonfinite reference accepted");
    // Errors leave the const adapter usable, with unchanged correlated noise.
    check_matrix(model.s_parameters(1e6),
                 oracle(saved, 1e6).scattering,
                 1.,
                 "S changed after invalid input");
    check_matrix(model.noise_correlation(1e6).watts_per_hz,
                 oracle(saved, 1e6).noise.watts_per_hz,
                 kt,
                 "C changed after invalid input");
    const auto retry =
        model.conversion_device(1e6, {1, 2, 3}, rf::ModelNoisePolicy::require_model_noise);
    require(retry.model.channels().size() == 6, "conversion retry failed");
}
} // namespace mna_noise_example
