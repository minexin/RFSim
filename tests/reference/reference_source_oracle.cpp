// Independent deterministic mathematical anchors, not a vendor oracle.
// No RFModel headers, implementation helpers, C ABI, JSON, or Python. Every row:
// name operation payload-token-count payload... expected-token-count expected...
// Expected tokens are the successful source-archive wire body. Phasor grouping,
// scalar compression/limiting and ordered signed Fourier products are computed
// here in long double, without calling the tested model or reduction routines.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using Wide = long double;
using Wave = std::complex<Wide>;
using SourceKey = std::tuple<int, double, std::uint64_t>;
using Inputs = std::map<SourceKey, Wave>;

struct Tone {
    int bin;
    double bandwidth;
    std::uint64_t group;
    double real;
    double imaginary;
};

struct Model {
    double gain = 20.;
    double p1db = 20.;
    double saturation = 23.;
    double iip2 = 20.;
    double iip3 = 10.;
    double reference = 50.;
    std::uint64_t reserved = 0;
};

struct Point {
    Wide gain;
    Wide scale;
    Wide limited;
};

std::ostringstream stream() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    return out;
}

void number(std::ostream &out, Wide value) {
    const auto rounded = static_cast<double>(value);
    if (!std::isfinite(rounded)) {
        throw std::runtime_error("oracle result not representable");
    }
    out << rounded;
}

void wave(std::ostream &out, Wave value) {
    number(out, value.real());
    out << ' ';
    number(out, value.imag());
}

std::size_t tokens(const std::string &value) {
    std::istringstream in(value);
    in.imbue(std::locale::classic());
    std::size_t count = 0;
    std::string token;
    while (in >> token) {
        ++count;
    }
    return count;
}

std::string row(const std::string &name,
                const std::string &operation,
                const std::string &payload,
                const std::string &expected) {
    auto out = stream();
    out << name << ' ' << operation << ' ' << tokens(payload) << ' ' << payload << ' '
        << tokens(expected) << ' ' << expected;
    return out.str();
}

Inputs reduced(const std::vector<Tone> &tones) {
    Inputs result;
    for (const auto &tone : tones) {
        result[{tone.bin, tone.bandwidth, tone.group}] += Wave(tone.real, tone.imaginary);
    }
    return result;
}

Wide drive(const Inputs &input) {
    Wide result = 0;
    for (const auto &entry : input) {
        result += std::norm(entry.second);
    }
    return result;
}

Point operating_point(const Model &m, Wide power) {
    // OP1dB is actual compressed output. The input anchor is OP1dB-G+1 dB.
    const Wide linear_gain = std::pow(Wide(10), Wide(m.gain) / 20);
    const Wide compression_at_anchor = std::pow(Wide(10), -Wide(1) / 20);
    const Wide input_anchor_power = std::pow(Wide(10), (Wide(m.p1db) - m.gain + 1 - 30) / 10);
    Wide gain = linear_gain * (1 - (1 - compression_at_anchor) * power / input_anchor_power);
    if (power > input_anchor_power) {
        const Wide output_anchor = std::sqrt(std::pow(Wide(10), (Wide(m.p1db) - 30) / 10));
        const Wide ceiling = std::sqrt(std::pow(Wide(10), (Wide(m.saturation) - 30) / 10));
        const Wide headroom = ceiling - output_anchor;
        const Wide slope = linear_gain * (3 * compression_at_anchor - 2);
        const Wide argument = slope * (std::sqrt(power) - std::sqrt(input_anchor_power)) / headroom;
        gain = (output_anchor + headroom * std::tanh(argument)) / std::sqrt(power);
    }
    // The nonlinear source limiter is a distinct scalar map from fundamental
    // compression. Its anchor and ceiling are fixed by this limited model.
    const Wide knee = std::sqrt(std::pow(Wide(10), (Wide(m.p1db) - m.gain - 34) / 10));
    const Wide ceiling = std::sqrt(std::pow(Wide(10), (Wide(m.saturation) - m.gain - 31) / 10));
    const Wide amplitude = std::sqrt(power);
    const Wide limited =
        amplitude <= knee
            ? amplitude
            : knee + (ceiling - knee) * std::tanh((amplitude - knee) / (ceiling - knee));
    return {gain, power == 0 ? 1 : limited / amplitude, limited * limited};
}

void component(
    std::ostream &out, int bin, int kind, double bandwidth, std::uint64_t group, Wave amplitude) {
    out << bin << ' ' << kind << ' ' << bandwidth << ' ' << group << ' ';
    wave(out, amplitude);
}

std::string payload(double spacing, const Model &m, const std::vector<Tone> &tones, bool cubic) {
    auto out = stream();
    out << spacing << ' ' << m.gain << ' ' << m.p1db << ' ' << m.saturation;
    if (cubic) {
        out << ' ' << m.iip2 << ' ' << m.iip3 << ' ' << m.reference << ' ' << m.reserved;
    }
    out << ' ' << tones.size();
    for (const auto &tone : tones) {
        out << ' ';
        component(out, tone.bin, 0, tone.bandwidth, tone.group, {tone.real, tone.imaginary});
    }
    return out.str();
}

std::string shared_case(const std::string &name,
                        const std::vector<Tone> &tones,
                        Model m = {},
                        double spacing = 1.e8) {
    const auto input = reduced(tones);
    const auto power = drive(input);
    const auto point = operating_point(m, power);
    auto expected = stream();
    number(expected, power);
    expected << ' ' << input.size();
    std::map<int, Wide> bin_power;
    Wide total = 0;
    for (const auto &entry : input) {
        const auto output = entry.second * point.gain;
        expected << ' ';
        component(expected,
                  std::get<0>(entry.first),
                  0,
                  std::get<1>(entry.first),
                  std::get<2>(entry.first),
                  output);
        bin_power[std::get<0>(entry.first)] += std::norm(output);
        total += std::norm(output);
    }
    expected << ' ' << bin_power.size();
    for (const auto &entry : bin_power) {
        expected << ' ' << entry.first << ' ';
        number(expected, entry.second);
    }
    expected << ' ';
    number(expected, total);
    return row(name, "shared_fundamentals_v1", payload(spacing, m, tones, false), expected.str());
}

using TermKey = std::tuple<int, int, std::array<int, 3>>;

struct Term {
    double bandwidth;
    Wave amplitude;
};

std::string cubic_case(const std::string &name,
                       const std::vector<Tone> &tones,
                       Model m = {},
                       double spacing = 1.e8) {
    const auto input = reduced(tones);
    const auto power = drive(input);
    const auto point = operating_point(m, power);
    const std::vector<std::pair<SourceKey, Wave>> sources(input.begin(), input.end());
    const Wide resistance = m.reference;
    const Wide voltage_scale = std::sqrt(resistance / 2);
    const Wide linear_gain = std::pow(Wide(10), Wide(m.gain) / 20);
    const Wide ip2 = std::pow(Wide(10), (Wide(m.iip2) - 30) / 10);
    const Wide ip3 = std::pow(Wide(10), (Wide(m.iip3) - 30) / 10);
    const std::array<Wide, 4> coefficients{0,
                                           0,
                                           linear_gain / std::sqrt(2 * resistance * ip2),
                                           -2 * linear_gain / (3 * resistance * ip3)};
    std::map<TermKey, Term> terms;
    std::uint64_t group = m.reserved;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto &entry = sources[i];
        group = std::max(group, std::get<2>(entry.first));
        terms.emplace(TermKey{1, std::get<0>(entry.first), {static_cast<int>(i + 1), 0, 0}},
                      Term{std::get<1>(entry.first), entry.second * point.gain});
    }

    struct Signed {
        int index;
        int bin;
        double bandwidth;
        Wave voltage;
    };

    std::vector<Signed> signed_sources;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto &entry = sources[i];
        if (entry.second == Wave{}) {
            continue;
        }
        for (const int sign : {-1, 1}) {
            signed_sources.push_back({sign * static_cast<int>(i + 1),
                                      sign * std::get<0>(entry.first),
                                      std::get<1>(entry.first),
                                      (sign > 0 ? entry.second : std::conj(entry.second)) *
                                          point.scale * voltage_scale});
        }
    }
    // Enumerate EVERY ordered signed tuple. Sorting its indices only selects
    // the origin bucket; summation supplies multiplicity without SDK formulas.
    for (const int order : {2, 3}) {
        std::function<void(int, int, double, std::array<int, 3>, Wave)> visit;
        visit =
            [&](int depth, int bin, double bandwidth, std::array<int, 3> indices, Wave voltage) {
                if (depth == order) {
                    if (bin <= 0 || voltage == Wave{}) {
                        return;
                    }
                    // A bounded sorting network avoids a runtime iterator end in
                    // this recursive lambda. Only degrees two and three exist.
                    if (indices[1] < indices[0]) {
                        std::swap(indices[0], indices[1]);
                    }
                    if (order == 3) {
                        if (indices[2] < indices[1]) {
                            std::swap(indices[1], indices[2]);
                        }
                        if (indices[1] < indices[0]) {
                            std::swap(indices[0], indices[1]);
                        }
                    }
                    auto &term = terms[{order, bin, indices}];
                    term.bandwidth = bandwidth;
                    term.amplitude +=
                        coefficients[static_cast<std::size_t>(order)] * voltage / voltage_scale;
                    return;
                }
                for (const auto &source : signed_sources) {
                    indices[static_cast<std::size_t>(depth)] = source.index;
                    visit(depth + 1,
                          bin + source.bin,
                          bandwidth + source.bandwidth,
                          indices,
                          voltage * source.voltage);
                }
            };
        visit(0, 0, 0., {}, {1, 0});
    }
    auto expected = stream();
    number(expected, power);
    expected << ' ';
    number(expected, point.limited);
    expected << ' ' << input.size();
    for (const auto &entry : input) {
        expected << ' ';
        component(expected,
                  std::get<0>(entry.first),
                  0,
                  std::get<1>(entry.first),
                  std::get<2>(entry.first),
                  entry.second);
    }
    expected << ' ' << terms.size();
    for (const auto &entry : terms) {
        const auto order = std::get<0>(entry.first);
        const auto bin = std::get<1>(entry.first);
        const auto indices = std::get<2>(entry.first);
        const int kind = order == 1                                           ? 0
                         : indices[0] > 0 && indices[0] == indices[order - 1] ? 1
                                                                              : 2;
        const auto term_group =
            order == 1 ? std::get<2>(sources[static_cast<std::size_t>(indices[0] - 1)].first)
                       : ++group;
        expected << ' ' << order;
        for (int i = 0; i < order; ++i) {
            expected << ' ' << indices[static_cast<std::size_t>(i)];
        }
        expected << ' ';
        component(expected, bin, kind, entry.second.bandwidth, term_group, entry.second.amplitude);
    }
    return row(name, "cubic_sources_v1", payload(spacing, m, tones, true), expected.str());
}

std::string groups_case(const std::string &name,
                        const std::vector<std::pair<std::string, std::string>> &definitions) {
    // Independent equivalence classes: free-running domains keyed by source,
    // locked domains keyed by clock. IDs reflect sorted complete domain keys.
    std::vector<std::pair<bool, std::string>> keys;
    for (const auto &definition : definitions) {
        keys.emplace_back(!definition.second.empty(),
                          definition.second.empty() ? definition.first : definition.second);
    }
    auto domains = keys;
    std::sort(domains.begin(), domains.end());
    domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
    auto request = stream();
    auto expected = stream();
    request << definitions.size();
    expected << definitions.size();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto clock = definitions[i].second.empty() ? "-" : definitions[i].second;
        request << ' ' << definitions[i].first << ' ' << clock;
        expected << ' ' << definitions[i].first << ' ' << clock << ' '
                 << 1 + std::distance(domains.begin(),
                                      std::find(domains.begin(), domains.end(), keys[i]));
    }
    return row(name, "source_groups_v1", request.str(), expected.str());
}
} // namespace

int main(int argc, char **) {
    try {
        if (argc != 1) {
            return 2;
        }
        std::vector<std::string> cases;
        cases.push_back(groups_case("groups-one", {{"Source1", ""}}));
        cases.push_back(groups_case("groups-independent", {{"Source1", ""}, {"Source2", ""}}));
        cases.push_back(groups_case("groups-reversed", {{"Source2", ""}, {"Source1", ""}}));
        cases.push_back(groups_case("groups-locked",
                                    {{"Source1", "RFModelClock"}, {"Source2", "RFModelClock"}}));
        cases.push_back(groups_case("groups-locked-reversed",
                                    {{"Source2", "RFModelClock"}, {"Source1", "RFModelClock"}}));
        cases.push_back(
            groups_case("groups-distinct-clocks", {{"Source1", "clock2"}, {"Source2", "clock1"}}));
        cases.push_back(groups_case("groups-mixed", {{"Source1", "clock"}, {"Source2", ""}}));
        const std::vector<std::array<double, 6>> archive{
            {-6, -6, 0, 90, 0, 0},    {-6, -6, 0, 0, 0, 1},    {-6, -6, 0, 180, 0, 1},
            {-6, -6, 0, 90, 0, 1},    {-6, -6, 0, 0, 1, 1},    {-6, -6, 0, 90, 1, 1},
            {-30, -30, 0, 0, 0, 1},   {-6, -26, 0, 0, 0, 1},   {-30, -30, 0, 180, 0, 1},
            {-6, -26, 0, 180, 0, 1},  {-6, -26, 0, 90, 0, 1},  {-30, -30, 0, 0, 1, 1},
            {-6, -26, 0, 0, 1, 1},    {-6, -26, 0, 180, 1, 1}, {-26, -6, 0, 90, 1, 1},
            {-30, -30, 0, 90, 1, 1},  {-6, -26, 0, 90, 1, 1},  {-30, -30, 0, 0, 0, 1},
            {-30, -30, 0, 180, 0, 1}, {-30, -30, 0, 0, 1, 1},  {-6, -6, 0, 180, 1, 1},
            {-30, -30, 0, 90, 1, 1},  {-6, -6, 0, 180, 1, 1}};
        for (std::size_t i = 0; i < archive.size(); ++i) {
            const auto &p = archive[i];
            std::vector<Tone> tones;
            for (int j = 0; j < 2; ++j) {
                const auto phase = p[static_cast<std::size_t>(2 + j)] * std::acos(-1.) / 180.;
                const auto amplitude =
                    std::polar(std::sqrt(std::pow(10., (p[j] - 30.) / 10.)), phase);
                tones.push_back({j == 0 || p[5] != 0 ? 10 : 11,
                                 1.,
                                 p[4] != 0 ? 1U : static_cast<unsigned>(j + 1),
                                 amplitude.real(),
                                 amplitude.imag()});
            }
            cases.push_back(shared_case("archive-shared-" + std::to_string(i + 1), tones));
            cases.push_back(cubic_case("archive-cubic-" + std::to_string(i + 1), tones));
        }
        const std::vector<std::pair<std::string, std::vector<Tone>>> synthetic{
            {"single-zero", {{10, 1., 1, 0., 0.}}},
            {"two-zero", {{10, 1., 1, 0., 0.}, {11, 1., 2, 0., 0.}}},
            {"zero-and-active", {{10, 1., 1, 0., 0.}, {11, 1., 2, .001, -.002}}},
            {"exact-coherent-cancel", {{10, 1., 1, .003, -.004}, {10, 1., 1, -.003, .004}}},
            {"independent-opposite", {{10, 1., 1, .003, -.004}, {10, 1., 2, -.003, .004}}},
            {"locked-different-frequency", {{10, 1., 1, .003, -.004}, {11, 1., 1, -.002, .001}}},
            {"different-bandwidth", {{10, 1., 1, .003, -.004}, {10, 2., 1, -.002, .001}}},
            {"reversed-inputs", {{11, 1., 2, -.002, .001}, {10, 1., 1, .003, -.004}}},
            {"input-collision", {{1, 1., 1, .003, -.004}, {2, 1., 2, -.002, .001}}},
            {"deep-saturation", {{10, 1., 1, .3, -.4}, {11, 1., 2, -.2, .1}}}};
        for (const auto &value : synthetic) {
            cases.push_back(shared_case("shared-" + value.first, value.second));
            cases.push_back(cubic_case("cubic-" + value.first, value.second));
        }
        Model other{13., 27., 31., 25., 18., 75., 100};
        const std::vector<Tone> pair{{10, 2., 8, .003, -.004}, {13, 3., 7, -.002, .001}};
        cases.push_back(shared_case("shared-other-parameters", pair, other, 2.e6));
        cases.push_back(cubic_case("cubic-reference75-reserved100", pair, other, 2.e6));
        other.reference = 1.;
        cases.push_back(cubic_case("cubic-reference1", pair, other, 2.e6));
        other.gain = -6.;
        cases.push_back(cubic_case("cubic-negative-gain", pair, other, 2.e6));
        const auto boundary = std::pow(10., (20. - 20. + 1. - 30.) / 10.);
        const std::vector<Tone> anchor{{10, 1., 1, std::sqrt(boundary), 0.}};
        cases.push_back(shared_case("shared-p1db-boundary", anchor));
        cases.push_back(cubic_case("cubic-p1db-boundary", anchor));
        cases.push_back(cubic_case("cubic-maximum-bin", {{715827882, 1., 1, .001, 0.}}, {}, 1.));
        std::cout.imbue(std::locale::classic());
        std::cout << "RFMODEL_SOURCE_ARCHIVE_ORACLE 1 " << cases.size() << '\n';
        for (const auto &value : cases) {
            std::cout << value << '\n';
        }
        return std::cout ? 0 : 5;
    } catch (const std::exception &) {
        std::cerr << "source archive mathematical oracle failed\n";
        return 5;
    }
}
