#include "rfmodel/io/touchstone_writer.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <iomanip>
#include <iostream>

using namespace rfmodel;

namespace {
TouchstoneData sample(std::size_t ports = 1) {
    TouchstoneData d;
    d.ports = ports;
    d.frequencies_hz = {0.};
    d.matrices = {SMatrix{ports, std::vector<Complex>(ports * ports)}};
    for (std::size_t i = 0; i < ports * ports; ++i) {
        d.matrices[0].values[i] = {static_cast<double>(2 * i + 1), -static_cast<double>(2 * i + 2)};
    }
    return d;
}

std::string serialize(const TouchstoneData &d) {
    std::ostringstream out;
    write_touchstone_v2(out, d);
    return out.str();
}

struct Buffer : std::stringbuf {
    int writes{}, syncs{};
    bool short_write{}, throwing{};

    std::streamsize xsputn(const char *s, std::streamsize n) override {
        ++writes;
        if (throwing) {
            throw std::runtime_error("streambuf failure");
        }
        return std::stringbuf::xsputn(s, short_write ? n / 2 : n);
    }

    int sync() override {
        ++syncs;
        return 0;
    }
};

struct Comma : std::numpunct<char> {
    char do_decimal_point() const override {
        return ',';
    }

    char do_thousands_sep() const override {
        return '.';
    }

    std::string do_grouping() const override {
        return "\3";
    }
};

void invalid(const TouchstoneData &d) {
    Buffer buffer;
    const std::string sentinel = "destination sentinel\n";
    buffer.str(sentinel);
    std::ostream out(&buffer);
    rejects<std::invalid_argument>([&] {
        write_touchstone_v2(out, d);
    });
    require(buffer.writes == 0 && out.good() && buffer.str() == sentinel,
            "invalid input touched output sentinel");
}

void goldens() {
    // Hand-written independent layout oracle from IBIS 2.0 pp.6-17, not reader output.
    require(serialize(sample()) ==
                "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 1\n"
                "[Number of Frequencies] 1\n[Reference] 50\n[Matrix Format] Full\n"
                "[Network Data]\n0 1 -2\n[End]\n",
            "one port golden");
    auto d = sample(2);
    d.reference_impedance_ohms = 75;
    d.reference_impedances_ohms = {31, 73};
    const std::string expected = "[Version] 2.0\n# Hz S RI R 75\n[Number of Ports] 2\n"
                                 "[Two-Port Data Order] 12_21\n[Number of Frequencies] 1\n"
                                 "[Reference] 31 73\n[Matrix Format] Full\n[Network Data]\n"
                                 "0 1 -2 3 -4 5 -6 7 -8\n[End]\n";
    require(serialize(d) == expected, "nonreciprocal two port golden");
    for (auto format :
         {TouchstoneFormat::MA, TouchstoneFormat::DB, static_cast<TouchstoneFormat>(99)}) {
        for (auto matrix : {TouchstoneMatrixFormat::Lower, TouchstoneMatrixFormat::Upper}) {
            d.format = format;
            d.matrix_format = matrix;
            d.version = static_cast<TouchstoneVersion>(99);
            d.two_port_order = static_cast<TouchstoneTwoPortOrder>(99);
            d.frequency_scale = std::numeric_limits<double>::quiet_NaN();
            require(serialize(d) == expected, "representation metadata lost matrix elements");
        }
    }
    require(serialize(sample(3)) ==
                "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 3\n"
                "[Number of Frequencies] 1\n[Reference] 50 50 50\n[Matrix Format] Full\n"
                "[Network Data]\n0 1 -2 3 -4 5 -6\n 7 -8 9 -10 11 -12\n"
                " 13 -14 15 -16 17 -18\n[End]\n",
            "asymmetric three port golden");
}

void imported_representations() {
    const char *path = "touchstone-writer-source.ts";
    for (const auto *format : {"MA", "DB"}) {
        for (const auto *matrix : {"Lower", "Upper"}) {
            {
                std::ofstream file(path);
                file << "[Version] 2.0\n# GHz S " << format << " R 75\n"
                     << "[Number of Ports] 2\n[Two-Port Data Order] 21_12\n"
                     << "[Number of Frequencies] 1\n[Matrix Format] " << matrix
                     << "\n[Network Data]\n1 "
                     << (std::string(format) == "MA" ? "1 0 10 0 100 0" : "0 0 20 0 40 0")
                     << "\n[End]\n";
            }
            const auto data = read_touchstone(path);
            require(serialize(data) == "[Version] 2.0\n# Hz S RI R 75\n[Number of Ports] 2\n"
                                       "[Two-Port Data Order] 12_21\n[Number of Frequencies] 1\n"
                                       "[Reference] 75 75\n[Matrix Format] Full\n[Network Data]\n"
                                       "1000000000 1 0 10 0 10 0 100 0\n[End]\n",
                    "parsed MA/DB triangular normalization");
        }
    }
    std::remove(path);
}

void validation() {
    auto test = [](auto mutate) {
        for (bool with_noise : {false, true}) {
            auto d = sample(2);
            if (with_noise) {
                d.noise_samples.push_back({0., 0., {}, 0.});
            }
            mutate(d);
            invalid(d);
        }
    };
    test([](auto &d) {
        d.ports = 0;
    });
    test([](auto &d) {
        d.ports = 1025;
    });
    test([](auto &d) {
        d.ports = (std::numeric_limits<std::size_t>::max)();
    });
    test([](auto &d) {
        d.frequencies_hz.clear();
    });
    test([](auto &d) {
        d.matrices.clear();
    });
    test([](auto &d) {
        d.matrices[0].ports = 1;
    });
    test([](auto &d) {
        d.matrices[0].values.pop_back();
    });
    test([](auto &d) {
        d.matrices[0].values.push_back({});
    });
    test([](auto &d) {
        d.frequencies_hz.push_back(0.);
        d.matrices.push_back(d.matrices[0]);
    });
    test([](auto &d) {
        d.frequencies_hz = {2., 1.};
        d.matrices.push_back(d.matrices[0]);
    });
    test([](auto &d) {
        d.reference_impedances_ohms = {50};
    });
    test([](auto &d) {
        d.reference_impedances_ohms = {50, 50, 50};
    });
    for (double bad : {-1.,
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        test([&](auto &d) {
            d.frequencies_hz[0] = bad;
        });
        test([&](auto &d) {
            d.reference_impedance_ohms = bad;
        });
        test([&](auto &d) {
            d.reference_impedances_ohms = {50, bad};
        });
        if (!std::isfinite(bad)) {
            test([&](auto &d) {
                d.matrices[0].values.back() = {bad, 0};
            });
            test([&](auto &d) {
                d.matrices[0].values.back() = {0, bad};
            });
        }
    }
    test([](auto &d) {
        d.reference_impedance_ohms = 0;
    });
    test([](auto &d) {
        d.reference_impedances_ohms = {0, 50};
    });
}

bool identical(double a, double b) {
    return a == b && std::signbit(a) == std::signbit(b);
}

TouchstoneData noise_sample() {
    auto d = sample(2);
    d.reference_impedance_ohms = 75.;
    d.reference_impedances_ohms = {31., 73.};
    d.frequencies_hz = {1e9, 3e9};
    d.matrices = {SMatrix{2, {{0., 0.}, {.25, 0.}, {2., 0.}, {.125, 0.}}}};
    d.matrices.push_back(d.matrices.front());
    d.noise_samples = {{1.5e9, 3., {.5, 0.}, 37.5}, {4e9, 4., {0., .25}, 75.}};
    return d;
}

// Independent tokenizer for the writer's two-port output. It does not use the
// production Touchstone reader, numeric parser, or polar conversion helpers.
struct NoiseText {
    std::vector<double> network_frequencies;
    std::vector<std::array<double, 5>> rows;
};

double decimal(const std::string &token) {
    require(std::string(std::localeconv()->decimal_point) == ".", "oracle C locale");
    char *end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    // A representable subnormal may set ERANGE; preserve the returned value.
    require(!token.empty() && end == token.c_str() + token.size() && std::isfinite(value),
            "invalid independent decimal token");
    return value;
}

template <std::size_t N> std::array<double, N> numeric_row(const std::string &line) {
    std::istringstream input(line);
    input.imbue(std::locale::classic());
    std::array<double, N> result{};
    for (auto &value : result) {
        std::string token;
        require(static_cast<bool>(input >> token), "missing numeric column");
        value = decimal(token);
    }
    std::string extra;
    require(!(input >> extra), "unexpected numeric column");
    return result;
}

std::size_t declared_count(const std::string &line, const std::string &keyword) {
    std::istringstream input(line.substr(keyword.size()));
    input.imbue(std::locale::classic());
    std::size_t count{};
    std::string extra;
    require(static_cast<bool>(input >> count) && count && !(input >> extra),
            "invalid independent declared count");
    return count;
}

NoiseText noise_text(const std::string &text) {
    NoiseText result;
    std::istringstream input(text);
    std::string line;
    const std::string network_keyword = "[Number of Frequencies]";
    const std::string noise_keyword = "[Number of Noise Frequencies]";
    std::size_t network_count{}, noise_count{};
    bool network = false, noise = false, ended = false;
    while (std::getline(input, line)) {
        require(!ended && !line.empty(), "content after End or blank canonical line");
        if (line.compare(0, network_keyword.size(), network_keyword) == 0) {
            require(!network && !network_count, "misplaced or duplicate network count");
            network_count = declared_count(line, network_keyword);
        } else if (line.compare(0, noise_keyword.size(), noise_keyword) == 0) {
            require(!network && !noise_count, "misplaced or duplicate noise count");
            noise_count = declared_count(line, noise_keyword);
        } else if (line == "[Network Data]") {
            require(!network && network_count && noise_count, "invalid network block position");
            network = true;
        } else if (line == "[Noise Data]") {
            require(network && !noise && result.network_frequencies.size() == network_count,
                    "invalid noise block position");
            noise = true;
        } else if (line == "[End]") {
            require(noise && result.rows.size() == noise_count, "invalid End or noise count");
            ended = true;
        } else if (noise) {
            const auto row = numeric_row<5>(line);
            require(row[0] >= 0. &&
                        (result.rows.empty() ? row[0] <= result.network_frequencies.back()
                                             : row[0] > result.rows.back()[0]),
                    "invalid independent noise frequency grid");
            result.rows.push_back(row);
        } else if (network) {
            const auto row = numeric_row<9>(line);
            require(row[0] >= 0. && (result.network_frequencies.empty() ||
                                     row[0] > result.network_frequencies.back()),
                    "invalid independent network frequency grid");
            result.network_frequencies.push_back(row[0]);
        } else {
            require(line.front() == '[' || line.front() == '#', "numeric row before network");
        }
    }
    require(ended && result.network_frequencies.size() == network_count &&
                result.rows.size() == noise_count,
            "missing canonical noise blocks or count mismatch");
    return result;
}

void noise_goldens() {
    // Hand-authored IBIS 2.0 noise layout and units, including distinct grids and
    // unequal S references. No expected bytes are obtained from a production API.
    const std::string expected =
        "[Version] 2.0\n# Hz S RI R 75\n[Number of Ports] 2\n"
        "[Two-Port Data Order] 12_21\n[Number of Frequencies] 2\n"
        "[Number of Noise Frequencies] 2\n[Reference] 31 73\n"
        "[Matrix Format] Full\n[Network Data]\n"
        "1000000000 0 0 0.25 0 2 0 0.125 0\n"
        "3000000000 0 0 0.25 0 2 0 0.125 0\n"
        "[Noise Data]\n1500000000 3 0.5 0 37.5\n4000000000 4 0.25 90 75\n[End]\n";
    auto d = noise_sample();
    d.version = TouchstoneVersion::Legacy;
    d.format = TouchstoneFormat::DB;
    d.matrix_format = TouchstoneMatrixFormat::Lower;
    d.two_port_order = TouchstoneTwoPortOrder::Order21_12;
    d.frequency_scale = std::numeric_limits<double>::quiet_NaN();
    require(serialize(d) == expected, "raw noise complete golden");
    const auto parsed = noise_text(expected);
    require(parsed.network_frequencies == std::vector<double>({1e9, 3e9}) &&
                parsed.rows == std::vector<std::array<double, 5>>(
                                   {{1.5e9, 3., .5, 0., 37.5}, {4e9, 4., .25, 90., 75.}}),
            "independent raw noise columns or grids");
    d.reference_impedances_ohms = {11., 97.};
    const auto different_references = serialize(d);
    require(different_references.find("# Hz S RI R 75\n") != std::string::npos &&
                different_references.find("[Reference] 11 97\n") != std::string::npos &&
                noise_text(different_references).rows == parsed.rows,
            "S references changed option R or raw noise");
    d.reference_impedance_ohms = 25.;
    require(noise_text(serialize(d)).rows == parsed.rows,
            "writer renormalized already-owned raw noise");

    // Ensure the independent oracle itself rejects extra/missing columns and a
    // wrong declared row count, rather than silently ignoring emitted payload.
    for (const auto *row : {"1500000000 3 0.5 0", "1500000000 3 0.5 0 37.5 9"}) {
        auto malformed = expected;
        const std::string original = "1500000000 3 0.5 0 37.5";
        malformed.replace(malformed.find(original), original.size(), row);
        rejects<std::runtime_error>([&] {
            noise_text(malformed);
        });
    }
    auto malformed = expected;
    const std::string count = "[Number of Noise Frequencies] 2";
    malformed.replace(malformed.find(count), count.size(), "[Number of Noise Frequencies] 3");
    rejects<std::runtime_error>([&] {
        noise_text(malformed);
    });
}

void imported_legacy_noise() {
    const char *path = "touchstone-writer-legacy-noise.s2p";
    {
        std::ofstream file(path);
        file << "# GHz S RI R 75\n1 0 0 2 0 0.25 0 0.125 0\n"
                "3 0 0 2 0 0.25 0 0.125 0\n1.5 3 0.5 0 0.5\n4 4 0.25 90 1\n";
    }
    auto d = read_touchstone(path);
    std::remove(path);
    require(d.version == TouchstoneVersion::Legacy && d.noise_samples.size() == 2 &&
                d.noise_samples[0].noise_resistance_ohms == 37.5 &&
                d.noise_samples[1].noise_resistance_ohms == 75.,
            "legacy reader did not normalize Rn once");
    d.reference_impedances_ohms = {31., 73.};
    const auto text = serialize(d);
    const auto parsed = noise_text(text);
    require(parsed.rows[0][0] == 1.5e9 && parsed.rows[1][0] == 4e9 && parsed.rows[0][4] == 37.5 &&
                parsed.rows[1][4] == 75.,
            "legacy noise export repeated resistance or frequency normalization");
    const char *v2_path = "touchstone-writer-normalized-noise.ts";
    {
        std::ofstream file(v2_path);
        file << text;
    }
    const auto roundtrip = read_touchstone(v2_path);
    std::remove(v2_path);
    require(roundtrip.version == TouchstoneVersion::V2_0 &&
                roundtrip.reference_impedance_ohms == 75. &&
                roundtrip.reference_impedances_ohms == std::vector<double>({31., 73.}) &&
                roundtrip.noise_samples.size() == 2 &&
                roundtrip.noise_samples[0].noise_resistance_ohms == 37.5 &&
                roundtrip.noise_samples[1].noise_resistance_ohms == 75.,
            "v2 noise reader interoperability");
}

void noise_raw_domain_and_phase() {
    auto d = sample(2);
    // Zero S21, negative Fmin, Rn == 0, and |Gamma| >= 1 are serializable raw
    // values. The writer must not apply a model's noise-fit/PSD/passivity tests.
    std::fill(d.matrices[0].values.begin(), d.matrices[0].values.end(), Complex{});
    d.frequencies_hz = {100.};
    const std::vector<Complex> gammas = {{0., 0.},
                                         {-0., -0.},
                                         {2., 0.},
                                         {-2., 0.},
                                         {-2., -0.},
                                         {0., 2.},
                                         {0., -2.},
                                         {.3, .4},
                                         {-.3, .4},
                                         {-.3, -.4},
                                         {.3, -.4}};
    const std::vector<double> magnitudes = {0., 0., 2., 2., 2., 2., 2., .5, .5, .5, .5};
    // Fixed geometric/high-precision oracles, never atan2 or production helpers.
    const std::vector<double> degrees = {0.,
                                         0.,
                                         0.,
                                         180.,
                                         -180.,
                                         90.,
                                         -90.,
                                         53.1301023541559787,
                                         126.8698976458440213,
                                         -126.8698976458440213,
                                         -53.1301023541559787};
    for (std::size_t i = 0; i < gammas.size(); ++i) {
        d.noise_samples.push_back({static_cast<double>(i), -3., gammas[i], 0.});
    }
    auto parsed = noise_text(serialize(d));
    for (std::size_t i = 0; i < gammas.size(); ++i) {
        const auto &row = parsed.rows[i];
        require(row[1] == -3. && row[4] == 0. &&
                    std::abs(row[2] - magnitudes[i]) <=
                        2. * std::numeric_limits<double>::epsilon() * magnitudes[i] &&
                    std::abs(row[3] - degrees[i]) < 4e-14,
                "raw domain or independent phase quadrant oracle");
        if (i < 2) {
            require(identical(row[2], 0.) && identical(row[3], 0.),
                    "zero Gamma must have positive zero magnitude and angle");
        }
    }
    d.noise_samples = {{100., -1., {1., 0.}, 0.}, {101., -2., {2., 0.}, 1.}};
    parsed = noise_text(serialize(d));
    require(parsed.rows[0][0] == 100. && parsed.rows[1][0] == 101.,
            "first noise boundary or later noise above S support rejected");
}

void noise_precision() {
    const auto tiny = std::numeric_limits<double>::denorm_min();
    const auto normal = std::numeric_limits<double>::min();
    const auto max = (std::numeric_limits<double>::max)();
    auto d = sample(2);
    d.frequencies_hz = {max};
    d.noise_samples = {{-0., -0., {}, -0.},
                       {tiny, -tiny, {tiny, 0.}, tiny},
                       {normal, -normal, {normal, 0.}, normal},
                       {1., std::nextafter(1., 2.), {1., 0.}, std::nextafter(1., 0.)},
                       {std::nextafter(1., 2.), -max, {max, 0.}, max},
                       {max, max, {max / 2., 0.}, max / 2.}};
    auto parsed = noise_text(serialize(d));
    for (std::size_t i = 0; i < d.noise_samples.size(); ++i) {
        const auto &source = d.noise_samples[i];
        const auto &row = parsed.rows[i];
        require(identical(row[0], source.frequency_hz) &&
                    identical(row[1], source.minimum_noise_figure_db) &&
                    identical(row[4], source.noise_resistance_ohms) &&
                    row[2] == source.optimum_source_reflection.real() && row[3] == 0.,
                "noise finite scalar max_digits10 or axis magnitude precision");
    }
    d.noise_samples = {{0., 0., {max, 0.}, 0.},
                       {1., 0., {-max, 0.}, 0.},
                       {2., 0., {-max, -0.}, 0.},
                       {3., 0., {1., tiny}, 0.},
                       {4., 0., {1., -tiny}, 0.},
                       {5., 0., {-max, tiny}, 0.},
                       {6., 0., {-max, -tiny}, 0.},
                       {7., 0., {tiny, max}, 0.},
                       {8., 0., {tiny, -max}, 0.}};
    parsed = noise_text(serialize(d));
    // 180/pi rounds the smallest positive radian to 57 subnormal degree units.
    // Near the negative real or imaginary axis, a finite angle can round to
    // +/-180 or +/-90; componentwise relative accuracy is not promised for the
    // tiny Cartesian component lost in that finite-angle rounding.
    const std::array<double, 9> axis_degrees = {
        0., 180., -180., 57. * tiny, -57. * tiny, 180., -180., 90., -90.};
    for (std::size_t i = 0; i < parsed.rows.size(); ++i) {
        require(identical(parsed.rows[i][3], axis_degrees[i]) &&
                    parsed.rows[i][2] == (i == 3 || i == 4 ? 1. : max),
                "axis extreme or representable subnormal angle rejected or changed");
    }
    // The fixed 3-4-5 triangle detects squared-norm underflow/overflow. The
    // smallest diagonal necessarily rounds its magnitude to one subnormal unit.
    d.noise_samples = {{0., 1., {tiny, tiny}, 1.},
                       {1., 1., {3. * tiny, 4. * tiny}, 1.},
                       {2., 1., {3e-200, 4e-200}, 1.},
                       {3., 1., {3e200, 4e200}, 1.}};
    parsed = noise_text(serialize(d));
    require(parsed.rows[0][2] == tiny && parsed.rows[0][3] == 45. && parsed.rows[1][2] == 5. * tiny,
            "subnormal magnitude or angle lost");
    const std::array<double, 4> magnitudes = {tiny, 5. * tiny, 5e-200, 5e200};
    for (std::size_t i = 1; i < parsed.rows.size(); ++i) {
        require(std::abs(parsed.rows[i][2] / magnitudes[i] - 1.) < 5e-16 &&
                    std::abs(parsed.rows[i][3] - 53.1301023541559787) < 4e-14,
                "scaled hypot or fixed phase oracle");
    }
    const char *path = "touchstone-writer-small-noise.ts";
    {
        std::ofstream file(path);
        file << serialize(d);
    }
    const auto roundtrip = read_touchstone(path);
    std::remove(path);
    require(roundtrip.noise_samples.size() == d.noise_samples.size(), "noise roundtrip count");
    for (std::size_t i = 0; i < d.noise_samples.size(); ++i) {
        const auto expected = d.noise_samples[i].optimum_source_reflection;
        const auto actual = roundtrip.noise_samples[i].optimum_source_reflection;
        const double scale = std::max(std::abs(expected.real()), std::abs(expected.imag()));
        const double tolerance = 16. * std::numeric_limits<double>::epsilon() * scale + 2. * tiny;
        require(std::abs(actual.real() - expected.real()) <= tolerance &&
                    std::abs(actual.imag() - expected.imag()) <= tolerance,
                "noise complex roundtrip exceeded magnitude-scaled tolerance");
    }
}

void noise_validation() {
    auto test = [](auto mutate) {
        auto d = noise_sample();
        mutate(d);
        invalid(d);
    };
    for (std::size_t ports : {std::size_t{1}, std::size_t{3}}) {
        auto d = sample(ports);
        d.noise_samples.push_back({0., 0., {}, 0.});
        invalid(d);
    }
    test([](auto &d) {
        d.noise_samples.front().frequency_hz = std::nextafter(3e9, 4e9);
    });
    for (double frequency : {1.5e9, 1.4e9}) {
        test([&](auto &d) {
            d.noise_samples.back().frequency_hz = frequency;
        });
    }
    for (double bad : {-1.,
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        // Last-row failures ensure all validation/conversion precedes any write.
        test([&](auto &d) {
            d.noise_samples.back().frequency_hz = bad;
        });
        test([&](auto &d) {
            d.noise_samples.back().noise_resistance_ohms = bad;
        });
        if (!std::isfinite(bad)) {
            test([&](auto &d) {
                d.noise_samples.back().minimum_noise_figure_db = bad;
            });
            test([&](auto &d) {
                d.noise_samples.back().optimum_source_reflection = {bad, 0.};
            });
            test([&](auto &d) {
                d.noise_samples.back().optimum_source_reflection = {0., bad};
            });
        }
    }
    test([](auto &d) {
        const auto max = (std::numeric_limits<double>::max)();
        d.noise_samples.back().optimum_source_reflection = {max, max};
    });
    for (double sign : {-1., 1.}) {
        test([&](auto &d) {
            // Nonzero imaginary component, but the ratio underflows before
            // atan2 can supply a nonzero angle: reject before destination I/O.
            d.noise_samples.back().optimum_source_reflection = {
                (std::numeric_limits<double>::max)(),
                sign * std::numeric_limits<double>::denorm_min()};
        });
        test([&](auto &d) {
            // The intermediate radian underflows even though direct high-
            // precision conversion to degrees could still yield a subnormal.
            // This detectable conversion loss is conservatively rejected.
            d.noise_samples.back().optimum_source_reflection = {1e308, sign * 1e-16};
        });
    }
}

void precision() {
    const auto max = (std::numeric_limits<double>::max)();
    auto d = sample();
    d.frequencies_hz = {-0.,
                        std::numeric_limits<double>::denorm_min(),
                        std::numeric_limits<double>::min(),
                        1.,
                        std::nextafter(1., 2.),
                        max};
    d.matrices.clear();
    for (double x : d.frequencies_hz) {
        d.matrices.push_back({1, {{x, -x}}});
    }
    d.reference_impedance_ohms = std::numeric_limits<double>::denorm_min();
    d.reference_impedances_ohms = {max};
    const auto text = serialize(d);
    // Independent C conversion oracle: this test never changes the initial C locale.
    // strtod may set ERANGE for a representable subnormal; compare the returned
    // value against the independent expected doubles instead of discarding it.
    require(std::string(std::localeconv()->decimal_point) == ".", "oracle C locale");
    std::istringstream values(text.substr(text.find("[Network Data]\n") + 15));
    values.imbue(std::locale::classic());
    for (std::size_t i = 0; i < d.frequencies_hz.size(); ++i) {
        double f{}, re{}, im{};
        for (double *value : {&f, &re, &im}) {
            std::string token;
            require(static_cast<bool>(values >> token), "extreme numeric extraction");
            char *end = nullptr;
            *value = std::strtod(token.c_str(), &end);
            require(end == token.c_str() + token.size() && std::isfinite(*value),
                    "independent decimal conversion");
        }
        require(identical(f, d.frequencies_hz[i]) &&
                    identical(re, d.matrices[i].values[0].real()) &&
                    identical(im, d.matrices[i].values[0].imag()),
                "extreme precision loss");
    }
    const char *path = "touchstone-writer-extremes.ts";
    {
        std::ofstream file(path);
        file << text;
    }
    const auto roundtrip = read_touchstone(path);
    std::remove(path);
    require(roundtrip.reference_impedance_ohms == d.reference_impedance_ohms &&
                roundtrip.reference_impedances_ohms == d.reference_impedances_ohms,
            "reference precision");
    for (std::size_t i = 0; i < d.frequencies_hz.size(); ++i) {
        require(
            identical(roundtrip.frequencies_hz[i], d.frequencies_hz[i]) &&
                identical(roundtrip.matrices[i].values[0].real(), d.matrices[i].values[0].real()) &&
                identical(roundtrip.matrices[i].values[0].imag(), d.matrices[i].values[0].imag()),
            "extreme reader roundtrip");
    }
}

void unchanged_input(TouchstoneData data) {
    data.format = TouchstoneFormat::DB;
    data.version = TouchstoneVersion::V2_0;
    data.matrix_format = TouchstoneMatrixFormat::Lower;
    data.frequency_scale = std::numeric_limits<double>::quiet_NaN();
    data.reference_impedance_ohms = 75.;
    data.reference_impedances_ohms = {31., 73.};
    const auto snapshot = data;
    auto verify = [&] {
        require(data.ports == snapshot.ports && std::isnan(data.frequency_scale) &&
                    data.format == snapshot.format && data.version == snapshot.version &&
                    data.matrix_format == snapshot.matrix_format &&
                    data.two_port_order == snapshot.two_port_order &&
                    data.reference_impedance_ohms == snapshot.reference_impedance_ohms &&
                    data.reference_impedances_ohms == snapshot.reference_impedances_ohms &&
                    data.frequencies_hz == snapshot.frequencies_hz &&
                    data.matrices.size() == snapshot.matrices.size() &&
                    data.noise_samples.size() == snapshot.noise_samples.size(),
                "input metadata mutated");
        for (std::size_t i = 0; i < data.matrices.size(); ++i) {
            require(data.matrices[i].ports == snapshot.matrices[i].ports &&
                        data.matrices[i].values == snapshot.matrices[i].values,
                    "input matrix mutated");
        }
        for (std::size_t i = 0; i < data.noise_samples.size(); ++i) {
            const auto &actual = data.noise_samples[i];
            const auto &expected = snapshot.noise_samples[i];
            require(
                identical(actual.frequency_hz, expected.frequency_hz) &&
                    identical(actual.minimum_noise_figure_db, expected.minimum_noise_figure_db) &&
                    identical(actual.optimum_source_reflection.real(),
                              expected.optimum_source_reflection.real()) &&
                    identical(actual.optimum_source_reflection.imag(),
                              expected.optimum_source_reflection.imag()) &&
                    identical(actual.noise_resistance_ohms, expected.noise_resistance_ohms),
                "input noise mutated");
        }
    };
    serialize(data);
    verify();
    Buffer buffer;
    buffer.short_write = true;
    std::ostream out(&buffer);
    rejects<std::ios_base::failure>([&] {
        write_touchstone_v2(out, data);
    });
    verify();
}

void streams(const TouchstoneData &data) {
    for (const auto mask : {std::ios::goodbit,
                            std::ios::badbit,
                            std::ios::failbit,
                            std::ios::eofbit,
                            std::ios::badbit | std::ios::failbit,
                            std::ios::badbit | std::ios::eofbit,
                            std::ios::failbit | std::ios::eofbit,
                            std::ios::badbit | std::ios::failbit | std::ios::eofbit}) {
        Buffer buffer, tied_buffer;
        std::ostream out(&buffer), tied(&tied_buffer);
        out.imbue(std::locale(std::locale::classic(), new Comma));
        out << std::hex << std::showpos << std::scientific << std::setprecision(3)
            << std::setfill('@');
        out.width(91);
        out.setf(std::ios::unitbuf);
        out.tie(&tied);
        out.exceptions(mask);
        auto locale = out.getloc();
        auto flags = out.flags();
        auto verify = [&] {
            require(out.getloc() == locale && out.flags() == flags && out.precision() == 3 &&
                        out.fill() == '@' && out.width() == 91 && out.exceptions() == mask &&
                        out.tie() == &tied && !buffer.syncs && !tied_buffer.syncs,
                    "caller stream state changed");
        };
        write_touchstone_v2(out, data);
        require(buffer.str() == serialize(data) && buffer.writes == 1, "not one canonical write");
        verify();
        buffer.short_write = true;
        rejects<std::ios_base::failure>([&] {
            write_touchstone_v2(out, data);
        });
        require(out.bad(), "short write missing badbit");
        verify();
        out.clear();
        buffer.short_write = false;
        buffer.throwing = true;
        rejects<std::ios_base::failure>([&] {
            write_touchstone_v2(out, data);
        });
        require(out.bad(), "throwing buffer missing badbit");
        verify();
        for (auto state : {std::ios::eofbit, std::ios::failbit, std::ios::badbit}) {
            out.exceptions(std::ios::goodbit);
            out.clear(state);
            try {
                out.exceptions(mask);
            } catch (const std::ios_base::failure &) {
            }
            const auto writes = buffer.writes;
            rejects<std::ios_base::failure>([&] {
                write_touchstone_v2(out, data);
            });
            require(out.rdstate() == state && writes == buffer.writes,
                    "initial failure touched output");
            verify();
        }
    }
    std::ostream null(nullptr);
    rejects<std::ios_base::failure>([&] {
        write_touchstone_v2(null, data);
    });
}

void budget() {
    {
        touchstone_writer_detail::Stage exact;
        const std::string chunk(1024, 'x');
        for (std::size_t n = 0; n < touchstone_v2_max_output_bytes / chunk.size(); ++n) {
            require(exact.sputn(chunk.data(), static_cast<std::streamsize>(chunk.size())) == 1024,
                    "stage short write before budget");
        }
        require(exact.size() == touchstone_v2_max_output_bytes, "exact budget not accepted");
        rejects<std::length_error>([&] {
            exact.sputc('x');
        });
        require(exact.size() == touchstone_v2_max_output_bytes, "overflow changed stage");
    }
    auto d = sample(1024);
    std::fill(d.matrices[0].values.begin(), d.matrices[0].values.end(), Complex{1., 0.});
    // Upper accepted port boundary with a small representation.
    require(serialize(d).find("[Number of Ports] 1024\n") != std::string::npos,
            "1024 ports rejected");
    d.matrices.push_back(d.matrices.front());
    d.frequencies_hz.push_back(1.);
    for (auto &m : d.matrices) {
        std::fill(m.values.begin(),
                  m.values.end(),
                  Complex{1.2345678901234567e200, -1.2345678901234567e200});
    }
    Buffer buffer;
    const std::string sentinel = "budget sentinel\n";
    buffer.str(sentinel);
    std::ostream out(&buffer);
    rejects<std::length_error>([&] {
        write_touchstone_v2(out, d);
    });
    require(buffer.writes == 0 && out.good() && buffer.str() == sentinel,
            "budget failure touched output sentinel");
}

void noise_budget() {
    auto d = sample(2);
    // Only one tiny S row. Each actual noise row has three independently known
    // 23-or-more-character scalars plus frequency, angle, separators and newline,
    // so more than budget/64 rows must overflow during real serialization.
    const auto count = touchstone_v2_max_output_bytes / 64 + 1;
    d.noise_samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        d.noise_samples.push_back({static_cast<double>(i),
                                   -1.2345678901234567e200,
                                   {1.2345678901234567e200, 0.},
                                   1.2345678901234567e200});
    }
    Buffer buffer;
    const std::string sentinel = "noise budget sentinel\n";
    buffer.str(sentinel);
    std::ostream out(&buffer);
    rejects<std::length_error>([&] {
        write_touchstone_v2(out, d);
    });
    require(buffer.writes == 0 && out.good() && buffer.str() == sentinel,
            "real noise payload overflow touched output sentinel");
}
} // namespace

int main() {
    rejects<std::runtime_error>([] {
        require(false, "checks must remain active with NDEBUG");
    });
    goldens();
    imported_representations();
    validation();
    precision();
    noise_goldens();
    imported_legacy_noise();
    noise_raw_domain_and_phase();
    noise_precision();
    noise_validation();
    unchanged_input(sample(2));
    unchanged_input(noise_sample());
    streams(sample());
    streams(noise_sample());
    budget();
    noise_budget();
    std::cout
        << "Touchstone S/noise writer goldens, validation, precision, streams and budget passed\n";
}
