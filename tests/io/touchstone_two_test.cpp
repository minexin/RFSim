#include "rfmodel/io/tabulated_noise_model.hpp"
#include "rfmodel/conversion/conversion_network.hpp"
#include "rfmodel/conversion/conversion_power_wave.hpp"
#include "rfmodel/linear/network.hpp"
#include "../support/test_support.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// Format oracle: IBIS Touchstone 2.0 (April 24, 2009), pp. 6-15 and 23-25.
// https://www.ibis.org/touchstone_ver2.0/touchstone_ver2_0.pdf
// All checks stay active under NDEBUG. Physical expectations below are derived
// from V/I and scalar wave equations, never a product renormalization function.
namespace {
using namespace rfmodel;
constexpr double kt = 1.380649e-23 * 290.;
std::size_t numerical_checks = 0;
std::size_t rejected_files = 0;
double maximum_scaled_error = 0.;

void check(Complex actual, Complex expected, double scale, const char *message) {
    require(std::isfinite(scale) && scale > 0., "invalid physical comparison scale");
    const double error = std::abs(actual - expected) / scale;
    if (!std::isfinite(error) || error > 3e-12) {
        std::ostringstream explanation;
        explanation << std::setprecision(17) << message << ": actual=" << actual
                    << ", expected=" << expected << ", scaled error=" << error;
        throw std::runtime_error(explanation.str());
    }
    maximum_scaled_error = std::max(maximum_scaled_error, error);
    ++numerical_checks;
}

void matrix(const SMatrix &actual, const SMatrix &expected, double scale, const char *message) {
    require(actual.ports == expected.ports && actual.values.size() == expected.values.size(),
            "matrix dimensions differ");
    for (std::size_t i = 0; i < expected.values.size(); ++i) {
        check(actual.values[i], expected.values[i], scale, message);
    }
}

struct Fixture {
    std::string path;

    explicit Fixture(const std::string &suffix) : path("touchstone-two-" + suffix) {
    }

    ~Fixture() {
        std::remove(path.c_str());
    }

    void write(const std::string &content) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
        require(static_cast<bool>(output), "cannot write Touchstone fixture");
    }

    TouchstoneData read(const std::string &content) const {
        write(content);
        return read_touchstone(path);
    }
};

std::string header(std::size_t ports = 2,
                   std::size_t frequencies = 1,
                   const std::string &extra = "",
                   const std::string &format = "RI",
                   const std::string &order = "21_12") {
    return "[Version] 2.0\n# GHz S " + format + " R 75\n[Number of Ports] " +
           std::to_string(ports) + "\n" +
           (ports == 2 ? "[Two-Port Data Order] " + order + "\n" : "") +
           "[Number of Frequencies] " + std::to_string(frequencies) + "\n" + extra +
           "[Network Data]\n";
}

std::string replace_once(std::string text, const std::string &from, const std::string &to) {
    const auto position = text.find(from);
    require(position != std::string::npos, "fixture replacement not found");
    text.replace(position, from.size(), to);
    return text;
}

void reject_file(const Fixture &file, const std::string &content, const char *label) {
    file.write(content);
    try {
        (void)read_touchstone(file.path);
    } catch (const std::runtime_error &) {
        ++rejected_files;
        return;
    }
    throw std::runtime_error(std::string("accepted invalid/unsupported fixture: ") + label);
}

void test_orders_formats_and_metadata() {
    const Fixture file("order.ts"), snp("order.S2P");
    const SMatrix expected{2, {{.1, .2}, {.3, -.4}, {.7, .5}, {-.2, .6}}};
    for (const std::string order : {"21_12", "12_21"}) {
        const std::string row =
            order == "21_12" ? "1 .1 .2 .7 .5 .3 -.4 -.2 .6\n" : "1 .1 .2 .3 -.4 .7 .5 -.2 .6\n";
        const auto content = header(2, 1, "", "RI", order) + row + "[End]\n";
        const auto data = file.read(content);
        require(data.version == TouchstoneVersion::V2_0, "version metadata");
        require(data.matrix_format == TouchstoneMatrixFormat::Full, "default Full metadata");
        require(data.two_port_order == (order == "21_12" ? TouchstoneTwoPortOrder::Order21_12
                                                         : TouchstoneTwoPortOrder::Order12_21),
                "two-port order metadata");
        require(data.ports == 2 && data.format == TouchstoneFormat::RI, "RI/port metadata");
        check(data.frequency_scale, 1e9, 1e9, "GHz scale");
        check(data.frequencies_hz[0], 1e9, 1e9, "GHz frequency");
        require(effective_reference_impedances(data) == std::vector<Complex>{75., 75.},
                "option reference fallback");
        check(common_reference_impedance(data), 75., 75., "common option reference");
        matrix(data.matrices[0], expected, 1., "nonreciprocal two-port order");
        matrix(snp.read(content).matrices[0], expected, 1., "version 2 in uppercase sNp");
    }
    // MA and DB independently describe the same four nonreciprocal complex entries.
    const SMatrix polar_expected{
        2, {Complex{0., 1.}, Complex{-2., 0.}, Complex{.5, 0.}, Complex{0., -.25}}};
    for (const std::string format : {"MA", "DB"}) {
        const auto row = format == "MA" ? "1 1 90 .5 0 2 180 .25 -90\n"
                                        : "1 0 90 -6.020599913279624 0 6.020599913279624 180 "
                                          "-12.041199826559248 -90\n";
        const auto data = file.read(header(2, 1, "", format) + row + "[End]\n");
        matrix(data.matrices[0], polar_expected, 2., "independent MA/DB matrix");
        require(data.format == (format == "MA" ? TouchstoneFormat::MA : TouchstoneFormat::DB),
                "data format metadata");
    }
    auto lowercase = header() + "1 0 0 2 0 0 0 0 0\n[End]\n";
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    check(file.read("! leading comment\r\n\r\n" + lowercase).matrices[0](1, 0),
          2.,
          2.,
          "case-insensitive keywords/options");
    const auto defaults = file.read("[Version] 2.0\n#\n[Number of Ports] 1\n"
                                    "[Number of Frequencies] 1\n[Network Data]\n"
                                    "1 .5 90 ! comment\n[End]\n! trailing comment\n");
    check(defaults.matrices[0](0, 0), Complex{0., .5}, 1., "option defaults");
    check(common_reference_impedance(defaults), 50., 50., "default reference");
    const auto legacy = snp.read("# GHz S RI R 75\n1 .1 .2 .7 .5 .3 -.4 -.2 .6\n");
    require(legacy.version == TouchstoneVersion::Legacy, "legacy metadata");
    matrix(legacy.matrices[0], expected, 1., "unchanged legacy ordering");
    require(effective_reference_impedances(legacy) == std::vector<Complex>{75., 75.},
            "legacy effective references");
}

void test_triangles_and_multiline_references() {
    const Fixture file("triangle.ts");
    const SMatrix expected{3,
                           {{.1, .2},
                            {.3, -.4},
                            {.5, .6},
                            {.3, -.4},
                            {-.2, .1},
                            {.7, -.8},
                            {.5, .6},
                            {.7, -.8},
                            {-.9, -.3}}};
    const std::array<std::string, 3> formats{"Full", "Lower", "Upper"};
    const std::array<std::string, 3> rows{
        "1 .1 .2 .3 -.4 .5 .6\n .3 -.4 -.2 .1 .7 -.8\n .5 .6 .7 -.8 -.9 -.3\n",
        "1 .1 .2\n .3 -.4 -.2 .1\n .5 .6 .7 -.8 -.9 -.3\n",
        "1 .1 .2 .3 -.4 .5 .6\n -.2 .1 .7 -.8\n -.9 -.3\n"};
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto data = file.read(
            header(3, 1, "[Reference]\n31 ! p1\n73 19\n[Matrix Format] " + formats[index] + "\n") +
            rows[index] + "[End]\n");
        matrix(data.matrices[0], expected, 1., "triangular is complex symmetric, not Hermitian");
        require(data.reference_impedances_ohms == std::vector<double>{31., 73., 19.},
                "multiline Reference order");
        require(effective_reference_impedances(data) == std::vector<Complex>{31., 73., 19.},
                "effective per-port references");
        const auto expected_format = index == 0   ? TouchstoneMatrixFormat::Full
                                     : index == 1 ? TouchstoneMatrixFormat::Lower
                                                  : TouchstoneMatrixFormat::Upper;
        require(data.matrix_format == expected_format, "matrix format metadata");
        rejects<std::invalid_argument>([&] {
            common_reference_impedance(data);
        });
        const TabulatedSParameterModel model("three port", data);
        for (std::size_t port = 0; port < 3; ++port) {
            check(model.port(port).reference_impedance,
                  data.reference_impedances_ohms[port],
                  75.,
                  "model exposes true reference");
        }
        rejects<std::out_of_range>([&] {
            model.port(3);
        });
    }
    // Two-port order only distinguishes off-diagonal entries of Full matrices.
    for (const std::string order : {"21_12", "12_21"}) {
        for (const std::string triangle : {"Lower", "Upper"}) {
            const auto data =
                file.read(header(2, 1, "[Matrix Format] " + triangle + "\n", "RI", order) +
                          "1 .1 .2 .3 -.4 -.2 .6\n[End]\n");
            matrix(data.matrices[0],
                   {2, {{.1, .2}, {.3, -.4}, {.3, -.4}, {-.2, .6}}},
                   1.,
                   "two-port symmetric triangular order");
        }
    }
    const auto common =
        file.read(header(2, 1, "[Reference] 50 50\n") + "1 0 0 1 0 1 0 0 0\n[End]\n");
    check(common.reference_impedance_ohms, 75., 75., "option R remains noise reference");
    check(common_reference_impedance(common), 50., 50., "explicit common Reference overrides R");
}

void test_cartesian_interpolation_with_unequal_references() {
    const Fixture file("cartesian.ts");
    const auto data = file.read(header(2, 2, "[Reference] 31 73\n", "MA", "12_21") +
                                "1 .8 90 .6 0 2 0 .3 90\n"
                                "3 .8 -90 .2 180 1 180 .7 -90\n[End]\n");
    const TabulatedSParameterModel model("unequal interpolation", data);
    matrix(model.s_parameters(1e9),
           {2, {Complex{0., .8}, .6, 2., Complex{0., .3}}},
           2.,
           "nonconstant MA first knot");
    matrix(model.s_parameters(3e9),
           {2, {Complex{0., -.8}, -.2, -1., Complex{0., -.7}}},
           2.,
           "nonconstant MA last knot");
    matrix(model.s_parameters(2e9),
           {2, {0., .2, .5, Complex{0., -.2}}},
           2.,
           "unequal-reference midpoint is Cartesian, not magnitude/phase");
    require(model.port(0).reference_impedance == Complex{31., 0.} &&
                model.port(1).reference_impedance == Complex{73., 0.},
            "interpolation preserved unequal coordinates");
}

std::string series_fixture() {
    // V1-V2=25*I1, I2=-I1. With references 31 and 73 ohms the
    // denominator is 25+31+73=129, and transmissions are 2*sqrt(31*73)/129.
    std::ostringstream rows;
    rows << std::setprecision(17);
    for (double frequency : {1., 3.}) {
        rows << frequency << ' ' << 67. / 129. << " 0 " << 2. * std::sqrt(2263.) / 129. << " 0 "
             << 2. * std::sqrt(2263.) / 129. << " 0 " << -17. / 129. << " 0\n";
    }
    return header(2, 2, "[Reference] 31 73\n") + rows.str() + "[End]\n";
}

void test_physical_series_resistor() {
    const Fixture file("resistor.ts");
    auto source_data = file.read(series_fixture());
    const TabulatedSParameterModel model("25 ohm resistor", source_data);
    const auto file_model = TabulatedSParameterModel::from_touchstone("snapshot", file.path);
    const SMatrix expected{
        2, {67. / 129., 2. * std::sqrt(2263.) / 129., 2. * std::sqrt(2263.) / 129., -17. / 129.}};
    source_data.matrices[0](0, 0) = 99.;
    source_data.reference_impedances_ohms[0] = 999.;
    file.write("invalid replacement\n");
    for (double f : {1e9, 2e9, 3e9}) {
        matrix(model.s_parameters(f), expected, 1., "series resistor knots and midpoint");
        matrix(file_model.s_parameters(f), expected, 1., "file snapshot ownership");
        LinearNetwork network;
        const std::vector<Complex> refs{model.port(0).reference_impedance,
                                        model.port(1).reference_impedance};
        network.add_power_waves(model.s_parameters(f), refs);
        const Complex vs{.7, -.2}, zs{45., 3.};
        network.terminate(0, (zs - refs[0]) / (zs + refs[0]), std::sqrt(31.) * vs / (zs + refs[0]));
        network.terminate(1, (50. - refs[1]) / (50. + refs[1]));
        const auto waves = network.solve();
        const Complex physical_current = vs / Complex{120., 3.};
        const std::array<Complex, 2> voltage{75. * physical_current, 50. * physical_current};
        const std::array<Complex, 2> current{physical_current, -physical_current};
        for (std::size_t p = 0; p < 2; ++p) {
            const double root = std::sqrt(refs[p].real());
            check(root * (waves.incident[p] + waves.outgoing[p]),
                  voltage[p],
                  .7,
                  "loaded resistor physical voltage");
            check((waves.incident[p] - waves.outgoing[p]) / root,
                  current[p],
                  .01,
                  "loaded resistor physical current");
            check(std::norm(waves.incident[p]) - std::norm(waves.outgoing[p]),
                  (voltage[p] * std::conj(current[p])).real(),
                  .01,
                  "loaded resistor absorbed power");
        }
    }
    rejects<std::out_of_range>([&] {
        model.s_parameters(.5e9);
    });
    rejects<std::out_of_range>([&] {
        model.s_parameters(4e9);
    });
    const TabulatedSParameterModel clamp("clamp", model.data(), OutOfBand::Clamp);
    matrix(clamp.s_parameters(0.), expected, 1., "low S clamp");
    matrix(clamp.s_parameters(4e9), expected, 1., "high S clamp");
    rejects<std::invalid_argument>([&] {
        model.s_parameters(-1.);
    });
    rejects<std::invalid_argument>([&] {
        model.s_parameters(std::numeric_limits<double>::quiet_NaN());
    });
    for (const auto &bad_refs :
         std::vector<std::vector<double>>{{31.},
                                          {31., 73., 19.},
                                          {0., 73.},
                                          {-1., 73.},
                                          {31., std::numeric_limits<double>::infinity()},
                                          {std::numeric_limits<double>::quiet_NaN(), 73.}}) {
        auto invalid = model.data();
        invalid.reference_impedances_ohms = bad_refs;
        rejects<std::invalid_argument>([&] {
            TabulatedSParameterModel bad("bad", invalid);
        });
    }
    auto legacy_shape = model.data();
    legacy_shape.reference_impedances_ohms.clear();
    require(effective_reference_impedances(legacy_shape) == std::vector<Complex>{75., 75.},
            "programmatic old-shape data remains supported");
}

struct AmplifierOracle {
    SMatrix scattering;
    SMatrix covariance;
};

AmplifierOracle amplifier_oracle(double r1, double r2) {
    // At 75 ohms b1=c1, b2=2*a1+c2 and C/kT=diag(1,4).
    // Independently substitute a75=A*aR+B*bR, b75=B*aR+A*bR,
    // where A=(sqrt(R/75)+sqrt(75/R))/2 and B has a minus sign.
    // Then L=[[1/A1,0],[2*B1/(A1*A2),1/A2]] takes old noise to new.
    const double a1 = .5 * (std::sqrt(r1 / 75.) + std::sqrt(75. / r1));
    const double b1 = .5 * (std::sqrt(r1 / 75.) - std::sqrt(75. / r1));
    const double a2 = .5 * (std::sqrt(r2 / 75.) + std::sqrt(75. / r2));
    const double b2 = .5 * (std::sqrt(r2 / 75.) - std::sqrt(75. / r2));
    const double l11 = 1. / a1, l21 = 2. * b1 / (a1 * a2), l22 = 1. / a2;
    return {
        {2, {-b1 / a1, 0., 2. / (a1 * a2), -b2 / a2}},
        {2, {kt * l11 * l11, kt * l11 * l21, kt * l11 * l21, kt * (l21 * l21 + 4. * l22 * l22)}}};
}

std::string amplifier_fixture(double r1, double r2, bool changing_noise = false) {
    const auto oracle = amplifier_oracle(r1, r2);
    std::ostringstream references, rows, noise;
    references << std::setprecision(17) << "[Reference] " << r1 << ' ' << r2
               << "\n[Number of Noise Frequencies] 2\n";
    rows << std::setprecision(17);
    for (double f : {1., 5.}) {
        rows << f;
        for (std::size_t index : {0u, 2u, 1u, 3u}) {
            rows << ' ' << oracle.scattering.values[index].real() << " 0";
        }
        rows << '\n';
    }
    noise << std::setprecision(17) << "[Noise Data]\n2 " << 10. * std::log10(2.) << " 0 0 37.5\n4 "
          << 10. * std::log10(changing_noise ? 4. : 2.) << " 0 0 "
          << (changing_noise ? 112.5 : 37.5) << "\n[End]\n";
    return header(2, 2, references.str()) + rows.str() + noise.str();
}

void test_noise_reference_semantics_and_loaded_chain() {
    const Fixture file("amplifier.ts"), legacy_file("amplifier.s2p");
    const auto common_data = file.read(amplifier_fixture(75., 75.));
    require(common_data.noise_samples.size() == 2, "explicit noise count");
    check(common_data.noise_samples[0].noise_resistance_ohms, 37.5, 75., "2.0 Rn is ohms");
    const auto common = TabulatedNoiseModel::from_data("common", common_data);
    const auto legacy_data =
        legacy_file.read("# GHz S RI R 75\n1 0 0 2 0 0 0 0 0\n5 0 0 2 0 0 0 0 0\n"
                         "2 3.010299956639812 0 0 .5\n4 3.010299956639812 0 0 .5\n");
    const auto legacy = TabulatedNoiseModel::from_data("legacy", legacy_data);
    check(legacy_data.noise_samples[0].noise_resistance_ohms, 37.5, 75., "legacy Rn normalized");
    for (double f : {2e9, 3e9, 4e9}) {
        matrix(common.noise_correlation(f).watts_per_hz,
               {2, {kt, 0., 0., 4. * kt}},
               kt,
               "independent common-reference C/kT diagonal");
        matrix(legacy.noise_correlation(f).watts_per_hz,
               common.noise_correlation(f).watts_per_hz,
               kt,
               "legacy and 2.0 noise units equivalent");
    }
    for (const auto &refs : std::vector<std::vector<double>>{{31., 73.}, {50., 50.}}) {
        auto input = file.read(amplifier_fixture(refs[0], refs[1]));
        const auto oracle = amplifier_oracle(refs[0], refs[1]);
        const auto model = TabulatedNoiseModel::from_data("unequal amplifier", input);
        const auto file_model = TabulatedNoiseModel::from_touchstone("owned file", file.path);
        input.matrices[0](1, 0) = 123.;
        input.reference_impedances_ohms[0] = 999.;
        input.noise_samples[0].noise_resistance_ohms = 999.;
        file.write("changed after construction\n");
        for (double f : {2e9, 3e9, 4e9}) {
            matrix(model.s_parameters(f), oracle.scattering, 2., "independent unequal S");
            matrix(model.noise_correlation(f).watts_per_hz,
                   oracle.covariance,
                   kt,
                   "noise GammaOpt remains at option R despite Reference override");
            matrix(file_model.noise_correlation(f).watts_per_hz,
                   oracle.covariance,
                   kt,
                   "noise model owns file snapshot");
            std::vector<Complex> actual_refs{model.port(0).reference_impedance,
                                             model.port(1).reference_impedance};
            require(actual_refs == std::vector<Complex>{refs[0], refs[1]}, "noise port references");
            auto lifted = lift_linear_conversion(f,
                                                 {1},
                                                 {model.s_parameters(f)},
                                                 {model.noise_correlation(f)},
                                                 std::vector<std::vector<Complex>>{actual_refs});
            const std::array<Complex, 2> q{Complex{.02, -.01}, Complex{-.03, .015}};
            std::array<double, 2> gain{}, gamma{};
            for (std::size_t p = 0; p < 2; ++p) {
                gain[p] = 2. * std::sqrt(50. * refs[p]) / (50. + refs[p]);
                gamma[p] = (50. - refs[p]) / (50. + refs[p]);
                lifted.source[p] = gain[p] * q[p];
                lifted.reflection[p] = gamma[p];
            }
            FrequencyConversionNetwork network(f);
            network.add_power_waves(lifted);
            const auto result = network.analyze(true);
            require(network.reference_impedances_ohms() == actual_refs, "lift retains references");
            // At physical 50-ohm terminations a75=-b75/5 for zero excitation,
            // hence b75=(c1,c2-2*c1/5). Multiplication by sqrt(24/25)
            // gives the outgoing physical 50-ohm covariance below.
            const SMatrix c50{2, {.96 * kt, -.384 * kt, -.384 * kt, 3.9936 * kt}};
            const std::array<Complex, 2> b50{.2 * q[0], 1.92 * q[0] + .2 * q[1]};
            for (std::size_t i = 0; i < 2; ++i) {
                const auto voltage = std::sqrt(50.) * (q[i] + b50[i]);
                const auto current = (q[i] - b50[i]) / std::sqrt(50.);
                check(result.incident[i],
                      (voltage + refs[i] * current) / (2. * std::sqrt(refs[i])),
                      .1,
                      "loaded imported incident wave");
                check(result.outgoing[i],
                      (voltage - refs[i] * current) / (2. * std::sqrt(refs[i])),
                      .1,
                      "loaded imported outgoing wave");
                check(result.net_noise_into_device_w_per_hz[i],
                      -c50(i, i),
                      kt,
                      "loaded physical 50-ohm net noise");
                for (std::size_t j = 0; j < 2; ++j) {
                    const auto expected = c50(i, j) / (gain[i] * gain[j]);
                    check(result.outgoing_noise.covariance(i, j),
                          expected,
                          kt,
                          "loaded complete outgoing covariance");
                    check(result.incident_noise.covariance(i, j),
                          gamma[i] * expected * gamma[j],
                          kt,
                          "loaded complete incident covariance");
                    check(result.incident_outgoing_noise.covariance(i, j),
                          gamma[i] * expected,
                          kt,
                          "loaded incident-outgoing cross covariance");
                    check(result.outgoing_noise.complementary(i, j),
                          0.,
                          kt,
                          "proper noise outgoing complementary covariance");
                    check(result.incident_noise.complementary(i, j),
                          0.,
                          kt,
                          "proper noise incident complementary covariance");
                    check(result.incident_outgoing_noise.complementary(i, j),
                          0.,
                          kt,
                          "proper noise cross complementary covariance");
                }
            }
        }
        matrix(model.s_parameters(3e9), oracle.scattering, 2., "network did not mutate owned S");
        matrix(model.noise_correlation(3e9).watts_per_hz,
               oracle.covariance,
               kt,
               "network did not mutate owned covariance");
    }
    // Noise format is always magnitude/degrees, independently of # RI.
    const auto angle =
        file.read(replace_once(amplifier_fixture(31., 73.), "0 0 37.5", ".5 90 37.5"));
    check(angle.noise_samples[0].optimum_source_reflection,
          Complex{0., .5},
          1.,
          "noise reflection uses magnitude/degrees");
}

void test_separate_grids_and_noise_policies() {
    const Fixture file("grids.ts");
    const auto data = file.read(amplifier_fixture(31., 73., true));
    const auto oracle = amplifier_oracle(31., 73.);
    const auto model = TabulatedNoiseModel::from_data("grids", data);
    const auto clamp = TabulatedNoiseModel::from_data("clamp", data, OutOfBand::Clamp);
    check(model.minimum_noise_frequency_hz(), 2e9, 1e9, "independent noise minimum");
    check(model.maximum_noise_frequency_hz(), 4e9, 1e9, "independent noise maximum");
    for (double f : {2e9, 3e9, 4e9}) {
        auto expected = oracle.covariance;
        for (auto &value : expected.values) {
            value *= f / 1e9 - 1.;
        }
        matrix(model.noise_correlation(f).watts_per_hz,
               expected,
               kt,
               "separate noise grid knots/linear covariance midpoint");
    }
    for (double f : {1e9, 5e9}) {
        matrix(model.s_parameters(f), oracle.scattering, 2., "S available beyond noise grid");
        rejects<std::out_of_range>([&] {
            model.noise_correlation(f);
        });
    }
    matrix(clamp.noise_correlation(0.).watts_per_hz, oracle.covariance, kt, "low noise clamp");
    auto last = oracle.covariance;
    for (auto &value : last.values) {
        value *= 3.;
    }
    matrix(clamp.noise_correlation(9e9).watts_per_hz, last, kt, "high noise clamp");
    matrix(clamp.s_parameters(9e9), oracle.scattering, 2., "high S clamp independent of noise");
    rejects<std::invalid_argument>([&] {
        model.noise_correlation(-1.);
    });
    rejects<std::invalid_argument>([&] {
        model.noise_correlation(std::numeric_limits<double>::infinity());
    });
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_data("temperature", data, OutOfBand::Reject, 0.);
    });
    auto no_noise = data;
    no_noise.noise_samples.clear();
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_data("empty", no_noise);
    });
    // A noise point outside S support is syntactically legal; model range policy
    // determines whether S can be obtained there for the noise conversion.
    const auto wider_noise =
        file.read(replace_once(amplifier_fixture(31., 73.), "\n4 3.010", "\n6 3.010"));
    rejects<std::out_of_range>([&] {
        TabulatedNoiseModel::from_data("outside S", wider_noise);
    });
    const auto wide_clamp =
        TabulatedNoiseModel::from_data("wide clamp", wider_noise, OutOfBand::Clamp);
    matrix(wide_clamp.noise_correlation(6e9).watts_per_hz,
           oracle.covariance,
           kt,
           "noise conversion clamps independent S grid");
}

void test_independent_passive_noise_oracle() {
    const Fixture file("thermal.ts");
    // A physical matched attenuator at 50 ohms has voltage transmission 1/2.
    // Its impedance matrix has diagonal 250/3 and off-diagonal 200/3 ohms.
    // Direct V/I elimination at 25/75 ohms gives the literal S below; passive
    // thermal equilibrium gives C/kT=I-S*S^H with these closed-form entries.
    std::ostringstream content;
    content << std::setprecision(17)
            << "[Version] 2.0\n# GHz S RI R 50\n[Number of Ports] 2\n"
               "[Two-Port Data Order] 12_21\n[Number of Frequencies] 1\n"
               "[Number of Noise Frequencies] 1\n[Reference] 25 75\n[Network Data]\n1 "
            << 23. / 61. << " 0 " << 16. * std::sqrt(3.) / 61. << " 0 " << 16. * std::sqrt(3.) / 61.
            << " 0 " << -17. / 61. << " 0\n[Noise Data]\n1 " << 10. * std::log10(4.)
            << " 0 0 46.875\n[End]\n";
    const auto data = file.read(content.str());
    const auto model = TabulatedNoiseModel::from_data("passive thermal oracle", data);
    matrix(model.noise_correlation(1e9).watts_per_hz,
           {2,
            {kt * 2424. / 3721.,
             -kt * 96. * std::sqrt(3.) / 3721.,
             -kt * 96. * std::sqrt(3.) / 3721.,
             kt * 2664. / 3721.}},
           kt,
           "independent passive thermal covariance");
}

void test_invalid_and_unsupported_files() {
    const Fixture file("invalid.ts"), snp("mismatch.s3p"), legacy_ts("legacy.ts"),
        unsupported("extension.txt"), legacy_controls("controls.s2p");
    for (char control : {'\v', '\f'}) {
        reject_file(legacy_controls,
                    "# GHz S RI R 50\n1 0 0 1 0 1 0 0 0\n" + std::string(1, control) + "\n",
                    "legacy control-only line must reject without accessing empty row");
    }
    const std::string valid = header() + "1 0 0 2 0 0 0 0 0\n[End]\n";
    const std::vector<std::array<std::string, 3>> mutations{
        {"version 1", "[Version] 2.0", "[Version] 1.0"},
        {"version 2.1", "[Version] 2.0", "[Version] 2.1"},
        {"version numeric approximation", "[Version] 2.0", "[Version] 2"},
        {"duplicate version", "[Version] 2.0", "[Version] 2.0\n[Version] 2.0"},
        {"missing version", "[Version] 2.0\n", ""},
        {"missing option", "# GHz S RI R 75\n", ""},
        {"duplicate option", "# GHz S RI R 75", "# GHz S RI R 75\n# GHz S RI R 75"},
        {"non-S data", "# GHz S RI R 75", "# GHz Z RI R 75"},
        {"unknown option field", "# GHz S RI R 75", "# GHz S RI R 75 extra"},
        {"unsupported units", "# GHz S RI R 75", "# THz S RI R 75"},
        {"invalid format", "# GHz S RI R 75", "# GHz S XX R 75"},
        {"zero option R", "# GHz S RI R 75", "# GHz S RI R 0"},
        {"missing ports", "[Number of Ports] 2\n", ""},
        {"duplicate ports", "[Number of Ports] 2", "[Number of Ports] 2\n[Number of Ports] 2"},
        {"zero ports", "[Number of Ports] 2", "[Number of Ports] 0"},
        {"fractional ports", "[Number of Ports] 2", "[Number of Ports] 2.5"},
        {"excess ports", "[Number of Ports] 2", "[Number of Ports] 1025"},
        {"missing two-port order", "[Two-Port Data Order] 21_12\n", ""},
        {"unknown two-port order", "21_12", "11_22"},
        {"duplicate order",
         "[Two-Port Data Order] 21_12",
         "[Two-Port Data Order] 21_12\n[Two-Port Data Order] 12_21"},
        {"missing count", "[Number of Frequencies] 1\n", ""},
        {"wrong count", "[Number of Frequencies] 1", "[Number of Frequencies] 2"},
        {"zero count", "[Number of Frequencies] 1", "[Number of Frequencies] 0"},
        {"fractional count", "[Number of Frequencies] 1", "[Number of Frequencies] 1.5"},
        {"duplicate count",
         "[Number of Frequencies] 1",
         "[Number of Frequencies] 1\n[Number of Frequencies] 1"},
        {"missing network marker", "[Network Data]\n", ""},
        {"duplicate network marker", "[Network Data]", "[Network Data]\n[Network Data]"},
        {"network marker argument", "[Network Data]", "[Network Data] extra"},
        {"missing end", "[End]\n", ""},
        {"end argument", "[End]", "[End] extra"},
        {"duplicate end", "[End]", "[End]\n[End]"},
        {"data after end", "[End]", "[End]\n2 0 0 2 0 0 0 0 0"},
        {"short record", "1 0 0 2 0 0 0 0 0", "1 0 0 2 0 0 0 0"},
        {"long record", "1 0 0 2 0 0 0 0 0", "1 0 0 2 0 0 0 0 0 7"},
        {"nonfinite wave", "1 0 0 2 0 0 0 0 0", "1 NaN 0 2 0 0 0 0 0"},
        {"nonfinite frequency", "1 0 0 2 0 0 0 0 0", "Inf 0 0 2 0 0 0 0 0"},
        {"negative frequency", "1 0 0 2 0 0 0 0 0", "-1 0 0 2 0 0 0 0 0"},
        {"bad numeric token", "1 0 0 2 0 0 0 0 0", "1 0bad 0 2 0 0 0 0 0"},
    };
    for (const auto &entry : mutations) {
        reject_file(file, replace_once(valid, entry[1], entry[2]), entry[0].c_str());
    }
    for (const std::string extra : {"[Reference] 31\n",
                                    "[Reference] 31 73 19\n",
                                    "[Reference] 0 73\n",
                                    "[Reference] -31 73\n",
                                    "[Reference] NaN 73\n",
                                    "[Reference] Inf 73\n",
                                    "[Reference] 31 73\n[Reference] 31 73\n",
                                    "[Matrix Format] Diagonal\n",
                                    "[Matrix Format] Lower\n[Matrix Format] Upper\n",
                                    "[Mixed-Mode Order] S1 S2\n",
                                    "[Begin Information]\nunsupported text\n[End Information]\n",
                                    "[Unknown] 1\n"}) {
        reject_file(file, header(2, 1, extra) + "1 0 0 2 0 0 0 0 0\n[End]\n", extra.c_str());
    }
    reject_file(file,
                header(1, 1, "[Two-Port Data Order] 21_12\n") + "1 0 0\n[End]\n",
                "two-port order on one-port");
    reject_file(file,
                header(2, 2) + "1 0 0 2 0 0 0 0 0\n1 0 0 2 0 0 0 0 0\n[End]\n",
                "duplicate S frequency must not become implicit noise");
    reject_file(file,
                header(2, 2) + "2 0 0 2 0 0 0 0 0\n1 0 0 2 0 0 0 0 0\n[End]\n",
                "decreasing S frequency");
    reject_file(snp, valid, "sNp port count mismatch");
    reject_file(legacy_ts, "# GHz S RI R 50\n1 0 0 1 0 1 0 0 0\n", "legacy .ts forbidden");
    reject_file(unsupported, valid, "unsupported extension");
    const std::string noise = amplifier_fixture(31., 73.);
    const std::vector<std::array<std::string, 3>> noise_mutations{
        {"missing noise count", "[Number of Noise Frequencies] 2\n", ""},
        {"wrong noise count", "[Number of Noise Frequencies] 2", "[Number of Noise Frequencies] 3"},
        {"zero noise count", "[Number of Noise Frequencies] 2", "[Number of Noise Frequencies] 0"},
        {"missing noise marker", "[Noise Data]\n", ""},
        {"duplicate noise marker", "[Noise Data]", "[Noise Data]\n[Noise Data]"},
        {"late first noise", "\n2 3.010", "\n6 3.010"},
        {"duplicate noise frequency", "\n4 3.010", "\n2 3.010"},
        {"negative reflection magnitude", "0 0 37.5", "-1 0 37.5"},
        {"negative noise Rn", "0 0 37.5", "0 0 -37.5"},
        {"nonfinite noise Rn", "0 0 37.5", "0 0 Inf"},
        {"short noise row", "0 0 37.5", "0 0"},
        {"long noise row", "0 0 37.5", "0 0 37.5 9"},
        {"noise continuation not supported", "0 0 37.5", "0 0\n37.5"},
    };
    for (const auto &entry : noise_mutations) {
        reject_file(file, replace_once(noise, entry[1], entry[2]), entry[0].c_str());
    }
    const auto unphysical = file.read(replace_once(noise, "0 0 37.5", "0 0 0"));
    rejects<std::invalid_argument>([&] {
        TabulatedNoiseModel::from_data("unphysical", unphysical);
    });
}

void test_decimal_number_boundaries() {
    // Exact binary literals are independent of both decimal parsing and the writer.
    const std::vector<std::pair<std::string, double>> cases = {
        {"+0", 0.},
        {"-0.0e+9999", -0.},
        {".5", 0x1p-1},
        {"1.", 1.},
        {"+1.25E+2", 125.},
        {"-2.5e-1", -0x1p-2},
        {"4.9406564584124654e-324", 0x1p-1074},
        {"+2.4703282292062328e-324", 0x1p-1074},
        {"-4.9406564584124654e-324", -0x1p-1074},
        {"9.8813129168249309e-324", 0x1p-1073},
        {"2.2250738585072009e-308", 0x0.fffffffffffffp-1022},
        {"2.2250738585072012e-308", 0x1p-1022},
        {"-2.2250738585072012e-308", -0x1p-1022},
        {"2.2250738585072014e-308", 0x1p-1022},
        {"1.7976931348623157e+308", 0x1.fffffffffffffp+1023},
        {"-1.7976931348623157e+308", -0x1.fffffffffffffp+1023},
        {"1.0000000000000002", 0x1.0000000000001p0},
    };
    Fixture file("numeric.s1p");
    for (const auto &entry : cases) {
        const auto actual =
            file.read("# Hz S RI R 50\n1 " + entry.first + " 0\n").matrices[0].values[0].real();
        require(actual == entry.second && std::signbit(actual) == std::signbit(entry.second),
                "decimal boundary mismatch");
    }
    for (const std::string token : {"",
                                    "+",
                                    "-",
                                    ".",
                                    "1e",
                                    "1e+",
                                    "1e-",
                                    "1e2x",
                                    "1.0.0",
                                    "1,5",
                                    "0x1p-1",
                                    "NaN",
                                    "Inf",
                                    "-Infinity",
                                    "1e309",
                                    "-1e309",
                                    "1.7976931348623159e308",
                                    "1e-9999",
                                    "-1e-9999",
                                    "2.4703282292062327e-324",
                                    " 1",
                                    "1 ",
                                    "1\t",
                                    "--1",
                                    "+-1"}) {
        rejects<std::runtime_error>([&] {
            touchstone_detail::number(token);
        });
    }
    rejects<std::runtime_error>([] {
        touchstone_detail::number(std::string("1\0x", 3));
    });

    // A comma-decimal C++ global locale must not affect numeric interpretation.
    struct Comma : std::numpunct<char> {
        char do_decimal_point() const override {
            return ',';
        }
    };

    const auto original = std::locale();

    struct Restore {
        std::locale original;

        ~Restore() {
            std::locale::global(original);
        }
    } restore{original};

    std::locale::global(std::locale(original, new Comma));
    require(touchstone_detail::number("+1.25e-1") == 0x1p-3, "numeric locale isolation");
    rejects<std::runtime_error>([] {
        touchstone_detail::number("1,25");
    });
}
} // namespace

int main() {
    try {
        test_decimal_number_boundaries();
        test_orders_formats_and_metadata();
        test_triangles_and_multiline_references();
        test_cartesian_interpolation_with_unequal_references();
        test_physical_series_resistor();
        test_noise_reference_semantics_and_loaded_chain();
        test_separate_grids_and_noise_policies();
        test_independent_passive_noise_oracle();
        test_invalid_and_unsupported_files();
        std::cout << std::setprecision(17)
                  << "{\"passed\":true,\"numerical_checks\":" << numerical_checks
                  << ",\"rejected_files\":" << rejected_files
                  << ",\"maximum_scaled_error\":" << maximum_scaled_error << "}\n";
    } catch (const std::exception &error) {
        std::cerr << "Touchstone 2.0 regression: " << error.what() << '\n';
        return 1;
    }
}
