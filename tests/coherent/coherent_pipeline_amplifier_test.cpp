#include "rfmodel/coherent/coherent_pipeline.hpp"
#include "rfmodel/coherent/coherent_amplifier.hpp"
#include "rfmodel/coherent/coherent_highorder_amplifier.hpp"
#include "../support/test_support.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace {
using namespace rfmodel;
using Wide = std::complex<long double>;
using Factors = std::vector<int>;
constexpr double spacing = 1e8;
constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

Wide wide(Complex value) {
    return {value.real(), value.imag()};
}

void close(Complex actual,
           Wide expected,
           const char *message,
           long double absolute = 3e-19L,
           long double relative = 3e-11L) {
    require(std::abs(wide(actual) - expected) <= absolute + relative * std::abs(expected), message);
}

void scalar(double actual,
            long double expected,
            const char *message,
            long double absolute = 3e-21L,
            long double relative = 3e-12L) {
    require(std::abs(static_cast<long double>(actual) - expected) <=
                absolute + relative * std::abs(expected),
            message);
}

Factors canonical(Factors factors) {
    std::sort(factors.begin(), factors.end(), [](int a, int b) {
        return std::make_pair(std::abs(a), a) < std::make_pair(std::abs(b), b);
    });
    return factors;
}

Factors labels(const MixingOrigin &origin) {
    Factors result;
    for (const auto &factor : origin) {
        require(factor.root_id <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
                "test root label is too large");
        result.push_back(static_cast<int>(factor.root_id) * factor.sign);
    }
    return canonical(std::move(result));
}

void append(Factors &destination, const Factors &source, int sign) {
    for (int factor : source) {
        destination.push_back(sign * factor);
    }
}

struct Key {
    int bin;
    SpectrumKind kind;
    Factors physical;

    bool operator<(const Key &other) const {
        return std::tie(bin, kind, physical) < std::tie(other.bin, other.kind, other.physical);
    }
};

struct Record {
    long double width{};
    std::map<Factors, Wide> history;
};

using Records = std::map<Key, Record>;

Wide amplitude(const Record &record) {
    Wide sum{};
    for (const auto &term : record.history) {
        sum += term.second;
    }
    return sum;
}

long double power(const Records &records) {
    long double sum = 0.;
    for (const auto &record : records) {
        // Different physical keys are independent even at the same RF bin.
        sum += std::norm(amplitude(record.second));
    }
    return sum;
}

void merge(Records &destination, const Records &source, Wide gain = {1.L, 0.L}) {
    for (const auto &entry : source) {
        auto &out = destination[entry.first];
        out.width = entry.second.width;
        for (const auto &term : entry.second.history) {
            out.history[term.first] += gain * term.second;
        }
    }
}

struct Parameters {
    double gain_db{8.};
    double p1_dbm{15.};
    double saturation_dbm{19.};
    double ip2_dbm{32.};
    double ip3_dbm{23.};
    double reference{50.};
};

struct Point {
    long double gain;
    long double scale;
    long double limited_power;
};

// Independent long-double scalar laws. No RFModel operating-point, gain,
// limiter, intercept conversion, reduction, or polynomial function is called.
Point point(const Parameters &p, long double drive) {
    const long double small_gain = std::pow(10.L, static_cast<long double>(p.gain_db) / 20.L);
    const long double p1in =
        std::pow(10.L, (static_cast<long double>(p.p1_dbm) - p.gain_db + 1.L - 30.L) / 10.L);
    const long double compression = std::pow(10.L, -.05L);
    long double gain = small_gain * (1.L - (1.L - compression) * drive / p1in);
    if (drive > p1in) {
        const long double p1out =
            std::pow(10.L, (static_cast<long double>(p.p1_dbm) - 30.L) / 10.L);
        const long double sat =
            std::pow(10.L, (static_cast<long double>(p.saturation_dbm) - 30.L) / 10.L);
        const long double headroom = std::sqrt(sat) - std::sqrt(p1out);
        const long double slope = small_gain * (3.L * compression - 2.L);
        gain = (std::sqrt(p1out) +
                headroom * std::tanh(slope * (std::sqrt(drive) - std::sqrt(p1in)) / headroom)) /
               std::sqrt(drive);
    }
    const long double knee =
        std::sqrt(std::pow(10.L, (static_cast<long double>(p.p1_dbm) - p.gain_db - 34.L) / 10.L));
    const long double ceiling = std::sqrt(
        std::pow(10.L, (static_cast<long double>(p.saturation_dbm) - p.gain_db - 31.L) / 10.L));
    const long double radius = std::sqrt(drive);
    const long double limited =
        radius <= knee ? radius
                       : knee + (ceiling - knee) * std::tanh((radius - knee) / (ceiling - knee));
    return {gain, drive == 0.L ? 1.L : limited / radius, limited * limited};
}

std::vector<long double> coefficients(const Parameters &p) {
    const long double gain = std::pow(10.L, static_cast<long double>(p.gain_db) / 20.L);
    const long double ip2 = std::pow(10.L, (static_cast<long double>(p.ip2_dbm) - 30.L) / 10.L);
    const long double ip3 = std::pow(10.L, (static_cast<long double>(p.ip3_dbm) - 30.L) / 10.L);
    return {0.L,
            0.L,
            gain / std::sqrt(2.L * p.reference * ip2),
            -2.L * gain / (3.L * p.reference * ip3)};
}

CoherentLimitedAmplifier model(const Parameters &p) {
    return {p.gain_db, p.p1_dbm, p.saturation_dbm, p.ip2_dbm, p.ip3_dbm, p.reference};
}

struct Counts {
    std::size_t physical{};
    std::size_t expanded{};
    std::size_t generated{};
    std::size_t discarded{};
    std::map<std::size_t, std::size_t> discarded_by_order;
    std::size_t direct{};
    std::size_t discarded_direct{};
};

struct Expected {
    Records records;
    Counts counts;
    long double input_power{};
    Point response{};
};

// An ordered signed-voltage convolution with independent test-local labels.
// All order! permutations are visited explicitly; no production multiplicity,
// polynomial enumeration, origin composition, or coherent reduction is reused.
Records generate(const Records &input,
                 const std::vector<long double> &coefficient,
                 long double scale,
                 long double reference,
                 std::size_t cutoff,
                 Counts &counts,
                 bool sources_only = true) {
    struct Input {
        Key key;
        long double width;
        Factors history;
        Wide voltage;
    };

    std::vector<Input> active;
    const long double voltage_scale = std::sqrt(reference / 2.L);
    for (const auto &record : input) {
        if (sources_only && record.first.kind != SpectrumKind::source) {
            continue;
        }
        ++counts.physical;
        counts.expanded += record.second.history.size();
        for (const auto &term : record.second.history) {
            if (term.second != Wide{}) {
                active.push_back({record.first,
                                  record.second.width,
                                  term.first,
                                  term.second * scale * voltage_scale});
            }
        }
    }
    Records output;
    std::set<std::vector<int>> candidates;
    std::map<std::vector<int>, std::size_t> discarded;
    for (std::size_t degree = 1; degree < coefficient.size(); ++degree) {
        if (coefficient[degree] == 0.L) {
            continue;
        }
        std::vector<int> selections;
        std::function<void(std::size_t, int, long double, Wide, Factors, Factors)> visit;
        visit = [&](std::size_t remaining,
                    int bin,
                    long double width,
                    Wide wave,
                    Factors physical,
                    Factors history) {
            if (remaining == 0) {
                if (bin <= 0 || wave == Wide{}) {
                    return;
                }
                auto path = selections;
                std::sort(path.begin(), path.end());
                candidates.insert(path);
                if (history.size() > cutoff) {
                    discarded.emplace(std::move(path), history.size());
                    return;
                }
                physical = canonical(std::move(physical));
                history = canonical(std::move(history));
                const bool harmonic = !physical.empty() && physical.front() > 0 &&
                                      std::all_of(physical.begin(), physical.end(), [&](int x) {
                                          return x == physical.front();
                                      });
                const auto kind =
                    degree == 1 ? active[static_cast<std::size_t>(selections.front() - 1)].key.kind
                    : harmonic  ? SpectrumKind::harmonic
                                : SpectrumKind::intermod;
                auto &record = output[{bin, kind, std::move(physical)}];
                record.width = width;
                record.history[std::move(history)] += wave * coefficient[degree] / voltage_scale;
                return;
            }
            for (std::size_t i = 0; i < active.size(); ++i) {
                for (int sign : {-1, 1}) {
                    const auto &term = active[i];
                    Factors next_physical = physical;
                    Factors next_history = history;
                    append(next_physical, term.key.physical, sign);
                    append(next_history, term.history, sign);
                    selections.push_back(sign * static_cast<int>(i + 1));
                    visit(remaining - 1,
                          bin + sign * term.key.bin,
                          width + term.width,
                          wave * (sign > 0 ? term.voltage : std::conj(term.voltage)),
                          std::move(next_physical),
                          std::move(next_history));
                    selections.pop_back();
                }
            }
        };
        visit(degree, 0, 0.L, Wide{1.L, 0.L}, {}, {});
    }
    counts.generated = candidates.size();
    counts.discarded = discarded.size();
    for (const auto &path : discarded) {
        ++counts.discarded_by_order[path.second];
    }
    return output;
}

Expected amplifier(const Records &input,
                   const Parameters &p,
                   const std::vector<long double> &coefficient,
                   std::size_t cutoff = unlimited) {
    Expected output;
    output.input_power = power(input);
    output.response = point(p, output.input_power);
    for (const auto &record : input) {
        for (const auto &term : record.second.history) {
            ++output.counts.direct;
            if (term.first.size() > cutoff) {
                ++output.counts.discarded_direct;
                continue;
            }
            auto &out = output.records[record.first];
            out.width = record.second.width;
            out.history[term.first] += term.second * output.response.gain;
        }
    }
    merge(output.records,
          generate(input, coefficient, output.response.scale, p.reference, cutoff, output.counts));
    return output;
}

CoherentComponent source(int bin, Complex wave, std::uint64_t group = 1, double width = .25) {
    return {bin, SpectrumKind::source, width, group, wave};
}

const StreamRecord &
unique(const StreamSnapshot &snapshot, int bin, SpectrumKind kind = SpectrumKind::source) {
    const StreamRecord *result = nullptr;
    for (const auto &record : snapshot.records) {
        if (record.component.bin == bin && record.component.kind == kind) {
            require(!result, "expected exactly one physical record at this bin/kind");
            result = &record;
        }
    }
    require(result, "expected physical record is missing");
    return *result;
}

Records seeds(const StreamSnapshot &snapshot, const std::vector<CoherentComponent> &inputs) {
    Records output;
    for (const auto &input : inputs) {
        const StreamRecord *found = nullptr;
        for (const auto &record : snapshot.records) {
            if (record.component.bin == input.bin &&
                record.component.coherence_group == input.coherence_group &&
                record.component.bandwidth_hz == input.bandwidth_hz) {
                found = &record;
            }
        }
        require(found && found->history.size() == 1 && found->physical_identity.factors.size() == 1,
                "seed must provide a primitive root with one history");
        // Only opaque root names are read from RFModel. Numerical inputs,
        // history algebra, physical keys, and power are built independently.
        auto &record = output[{input.bin, input.kind, labels(found->physical_identity.factors)}];
        record.width = input.bandwidth_hz;
        record.history[labels(found->history.front().factors)] += wide(input.amplitude);
    }
    return output;
}

void check_records(const StreamSnapshot &snapshot, const Records &expected) {
    require(snapshot.records.size() == expected.size(), "physical output record count differs");
    for (const auto &actual : snapshot.records) {
        const Key key{
            actual.component.bin, actual.component.kind, labels(actual.physical_identity.factors)};
        const auto found = expected.find(key);
        require(found != expected.end(), "unexpected physical identity, RF bin, or spectral kind");
        scalar(actual.component.bandwidth_hz,
               found->second.width,
               "independent unsigned support bandwidth");
        require(actual.history.size() == found->second.history.size(),
                "normalized history monomial count differs");
        long double magnitude_sum = 0.L;
        for (const auto &term : found->second.history) {
            magnitude_sum += std::abs(term.second);
        }
        close(actual.component.amplitude,
              amplitude(found->second),
              "independent physical aggregate",
              3e-19L + 2e-14L * magnitude_sum);
        for (const auto &term : actual.history) {
            const auto wanted = found->second.history.find(labels(term.factors));
            require(wanted != found->second.history.end(), "unexpected signed source history");
            close(term.amplitude, wanted->second, "independent signed-history contribution");
        }
    }
    scalar(snapshot.reduction.total_power_w,
           power(expected),
           "independent physical output power",
           2e-20L,
           8e-11L);
}

void check_result(const CoherentPipeline &pipeline,
                  const CoherentPipeline::AmplifierResult &actual,
                  const Expected &expected,
                  const std::vector<long double> &coefficient) {
    scalar(actual.input_power_w, expected.input_power, "complete common input drive");
    scalar(actual.operating_point.fundamental_amplitude_gain,
           expected.response.gain,
           "independent common fundamental gain");
    scalar(actual.operating_point.nonlinear_input_scale,
           expected.response.scale,
           "independent limiter input scale");
    scalar(actual.operating_point.limited_input_power_w,
           expected.response.limited_power,
           "independent limiter power");
    scalar(actual.operating_point.quadratic_voltage_coefficient,
           coefficient.size() > 2 ? coefficient[2] : 0.L,
           "independent a2 coefficient");
    scalar(actual.operating_point.cubic_voltage_coefficient,
           coefficient.size() > 3 ? coefficient[3] : 0.L,
           "independent a3 coefficient");
    require(actual.direct_contribution_count == expected.counts.direct,
            "direct count must count normalized contributions, including zeros");
    require(actual.discarded_direct_contributions == expected.counts.discarded_direct,
            "direct cutoff removal count");
    require(actual.nonlinear_counts.physical_input_count == expected.counts.physical,
            "generating physical source count");
    require(actual.nonlinear_counts.expanded_input_count == expected.counts.expanded,
            "generating expanded history count");
    require(actual.nonlinear_counts.generated_term_count == expected.counts.generated,
            "independent generated candidate path count");
    require(actual.nonlinear_counts.discarded_term_count == expected.counts.discarded,
            "independent discarded candidate path count");
    require(actual.nonlinear_counts.discarded_by_source_order == expected.counts.discarded_by_order,
            "independent source-order discard histogram");
    check_records(pipeline.inspect(actual.output), expected.records);
}

LinearNetwork transfer(Complex gain, double reference = 50.) {
    LinearNetwork network(reference);
    network.add({2, {0., 0., gain, 0.}});
    return network;
}

LinearNetwork combiner(Complex left, Complex right) {
    LinearNetwork network(50.);
    network.add({3, {0., 0., 0., 0., 0., 0., left, right, 0.}});
    return network;
}

void source_only_gain_limiter_and_reference_sweep() {
    for (double reference : {50., 75.}) {
        Parameters p;
        p.reference = reference;
        const long double knee = std::pow(10.L, (p.p1_dbm - p.gain_db - 34.L) / 10.L);
        const long double p1 = std::pow(10.L, (p.p1_dbm - p.gain_db + 1.L - 30.L) / 10.L);
        const long double saturation = std::pow(10.L, (p.saturation_dbm - p.gain_db - 31.L) / 10.L);
        const auto coefficient = coefficients(p);
        for (long double requested : {0.L,
                                      knee / 2.L,
                                      knee,
                                      knee * 1.01L,
                                      p1 * .99L,
                                      p1,
                                      p1 * 1.01L,
                                      saturation * .99L,
                                      saturation,
                                      saturation * 1.01L,
                                      10.L}) {
            CoherentPipeline pipeline(spacing, reference, 2);
            const Complex wave = std::polar(static_cast<double>(std::sqrt(requested)), .713);
            const std::vector<CoherentComponent> inputs{source(7, wave)};
            const auto seed = pipeline.seed(inputs);
            const auto expected = amplifier(seeds(pipeline.inspect(seed), inputs), p, coefficient);
            const auto usage = pipeline.usage();
            const auto output = pipeline.limited_amplifier(seed, model(p));
            check_result(pipeline, output, expected, coefficient);
            require(pipeline.usage().stream_count == usage.stream_count + 1 &&
                        pipeline.usage().transformation_count == usage.transformation_count + 1,
                    "one amplifier must commit one stream and one transformation");
            if (requested != 0.L) {
                require(pipeline.inspect(output.output).records.size() == 4,
                        "single tone needs source, H2, H3, and separate carrier-bin IM3");
                const auto &carrier = unique(pipeline.inspect(output.output), 7);
                const auto &nonlinear =
                    unique(pipeline.inspect(output.output), 7, SpectrumKind::intermod);
                require(carrier.component.coherence_group != nonlinear.component.coherence_group,
                        "calibrated direct and nonlinear carrier identities must stay separate");
                require(nonlinear.physical_identity.factors.size() == 3,
                        "positive/negative pairs must not cancel out of physical origin");
            }
        }
    }
}

void independent_groups_and_two_tone_phases() {
    for (double reference : {50., 75.}) {
        for (bool equal_bin : {false, true}) {
            Parameters p;
            p.reference = reference;
            CoherentPipeline pipeline(spacing, reference, 2);
            const std::vector<CoherentComponent> inputs{
                source(8, {.11, .037}, 1), source(equal_bin ? 8 : 12, {-.11, -.037}, 2)};
            const auto seed = pipeline.seed(inputs);
            const auto coefficient = coefficients(p);
            const auto expected = amplifier(seeds(pipeline.inspect(seed), inputs), p, coefficient);
            const auto limited = pipeline.limited_amplifier(seed, model(p));
            check_result(pipeline, limited, expected, coefficient);
            require(limited.input_power_w > .02,
                    "independent opposite-phase roots cannot cancel drive");
            require(limited.operating_point.nonlinear_input_scale < 1.,
                    "fixture must engage limiter");
            const std::vector<double> nonlinear{static_cast<double>(coefficient[2]),
                                                static_cast<double>(coefficient[3])};
            const CoherentHighOrderAmplifier high(
                p.gain_db, p.p1_dbm, p.saturation_dbm, nonlinear, reference);
            const auto high_result = pipeline.highorder_amplifier(seed, high, 3);
            // This also has the independent oracle; equality of two adapters alone is insufficient.
            check_result(pipeline, high_result, expected, coefficient);
            if (!equal_bin) {
                require(pipeline.inspect(limited.output).records.size() == 16,
                        "two-tone cubic fixture must contain 16 physical outputs");
                require(limited.nonlinear_counts.generated_term_count == 14,
                        "two-tone cubic fixture has 14 positive RF nonlinear paths");
            }
        }
    }
}

void two_and_three_stage_cascade() {
    CoherentPipeline pipeline(spacing, 50., 2);
    const std::vector<CoherentComponent> inputs{source(8, {.06, .025}, 1),
                                                source(12, {.045, -.01}, 2)};
    auto stream = pipeline.seed(inputs);
    Records expected = seeds(pipeline.inspect(stream), inputs);
    std::map<Key, std::uint64_t> first_groups;
    const std::vector<Parameters> stages{{10., 20., 23., 30., 20., 50.},
                                         {-1., 20., 23., 41., 31., 50.},
                                         {4., 15., 18., 32., 20., 50.}};
    for (std::size_t stage = 0; stage < stages.size(); ++stage) {
        const auto &p = stages[stage];
        const auto coefficient = coefficients(p);
        const auto wanted = amplifier(expected, p, coefficient);
        const auto output =
            pipeline.limited_amplifier(stream,
                                       model(p),
                                       stage == 0 ? AmplifierInputMode::sources_only
                                                  : AmplifierInputMode::propagate_distortion);
        check_result(pipeline, output, wanted, coefficient);
        const auto &snapshot = pipeline.inspect(output.output);
        require(snapshot.records.size() == 16, "cascade must reconcile conducted and new products");
        require(output.nonlinear_counts.generated_term_count == 14 &&
                    output.nonlinear_counts.physical_input_count == 2,
                "conducted distortion must never become a nonlinear generator");
        for (const auto &record : snapshot.records) {
            const Key key{record.component.bin,
                          record.component.kind,
                          labels(record.physical_identity.factors)};
            if (stage == 0) {
                first_groups.emplace(key, record.component.coherence_group);
            } else {
                require(first_groups.at(key) == record.component.coherence_group,
                        "H2/IM3/direct physical groups must survive every cascade stage");
            }
        }
        if (stage > 0) {
            long double source_power = 0.L;
            for (const auto &record : expected) {
                if (record.first.kind == SpectrumKind::source) {
                    source_power += std::norm(amplitude(record.second));
                }
            }
            require(output.input_power_w > source_power,
                    "all conducted distortion power must enter the common drive");
        }
        stream = output.output;
        expected = wanted.records;
    }
}

void parallel_branches_reuse_identities() {
    CoherentPipeline pipeline(spacing, 50., 2);
    std::vector<CoherentComponent> inputs{source(8, {.02, .004}, 1), source(12, {.013, -.006}, 2)};
    const auto seed = pipeline.seed(inputs);
    const auto original = seeds(pipeline.inspect(seed), inputs);
    std::reverse(inputs.begin(), inputs.end());
    const auto reordered = pipeline.seed(inputs);
    const Parameters lp{8., 15., 19., 32., 23., 50.};
    const Parameters rp{3., 13., 17., 29., 21., 50.};
    const auto right = pipeline.limited_amplifier(reordered, model(rp));
    const auto left = pipeline.limited_amplifier(seed, model(lp));
    const auto right_expected = amplifier(original, rp, coefficients(rp));
    const auto left_expected = amplifier(original, lp, coefficients(lp));
    check_result(pipeline, right, right_expected, coefficients(rp));
    check_result(pipeline, left, left_expected, coefficients(lp));
    std::map<Key, std::uint64_t> groups;
    for (const auto &record : pipeline.inspect(left.output).records) {
        groups.emplace(Key{record.component.bin,
                           record.component.kind,
                           labels(record.physical_identity.factors)},
                       record.component.coherence_group);
    }
    for (const auto &record : pipeline.inspect(right.output).records) {
        require(groups.at({record.component.bin,
                           record.component.kind,
                           labels(record.physical_identity.factors)}) ==
                    record.component.coherence_group,
                "parallel branch order/model parameters must not change physical identity");
    }
    const Complex lg{.4, .2}, rg{-.1, .7};
    const auto sum =
        pipeline.linear_network({{left.output, 0}, {right.output, 1}}, {0, 1, 2}, 2, [&](double) {
            return combiner(lg, rg);
        });
    Records combined;
    merge(combined, left_expected.records, wide(lg));
    merge(combined, right_expected.records, wide(rg));
    check_records(pipeline.inspect(sum), combined);
    require(pipeline.inspect(sum).records.size() == 16, "parallel products must reconcile");
    check_result(pipeline, left, left_expected, coefficients(lp));
    check_result(pipeline, right, right_expected, coefficients(rp));
}

struct Folded {
    CoherentPipeline::Stream stream;
    Records expected;
};

Records mixed_reference(const CoherentPipeline &pipeline,
                        const StreamSnapshot &actual,
                        const Records &input,
                        int lo_bin,
                        std::uint64_t lo_group,
                        double phase,
                        double gain_db = 0.) {
    int lo = 0;
    for (const auto &record : actual.records) {
        for (const auto &term : record.history) {
            for (const auto &factor : term.factors) {
                const auto root = pipeline.history_root(factor.root_id);
                if (root.role == RootRole::lo && root.bin == lo_bin &&
                    root.coherence_group == lo_group) {
                    lo = static_cast<int>(root.id);
                }
            }
        }
    }
    require(lo != 0, "mixer must retain the expected LO history root");
    const Wide pump = std::polar(1.L, static_cast<long double>(phase));
    const long double gain = std::pow(10.L, static_cast<long double>(gain_db) / 20.L);
    Records expected;
    for (const auto &entry : input) {
        for (bool difference : {true, false}) {
            const int bin =
                difference ? std::abs(entry.first.bin - lo_bin) : entry.first.bin + lo_bin;
            const auto &physical = unique(actual, bin, entry.first.kind);
            require(physical.physical_identity.factors.size() == 1,
                    "mixer must rebase physical identity to a primitive root");
            const auto primitive =
                pipeline.physical_root(physical.physical_identity.factors.front().root_id);
            require(primitive.role == RootRole::mixed && primitive.bin == bin,
                    "mixed physical root metadata");
            auto &out =
                expected[{bin, entry.first.kind, labels(physical.physical_identity.factors)}];
            out.width = entry.second.width;
            for (const auto &term : entry.second.history) {
                const int sign = difference && entry.first.bin < lo_bin ? -1 : 1;
                const int lo_sign = difference && entry.first.bin > lo_bin ? -1 : 1;
                Factors history;
                append(history, term.first, sign);
                history.push_back(lo_sign * lo);
                out.history[canonical(std::move(history))] +=
                    gain * (sign > 0 ? term.second : std::conj(term.second)) *
                    (lo_sign > 0 ? pump : std::conj(pump));
            }
        }
    }
    return expected;
}

void independent_rf_and_lo_at_equal_bins() {
    for (const auto &independent : {std::pair<bool, bool>{true, false},
                                    std::pair<bool, bool>{false, true},
                                    std::pair<bool, bool>{true, true}}) {
        CoherentPipeline pipeline(spacing, 50., 4);
        const std::vector<CoherentComponent> first{source(8, {.13, .02}, 1)};
        const std::vector<CoherentComponent> second{
            source(8, {-.13, -.02}, independent.first ? 4 : 1)};
        const auto left_seed = pipeline.seed(first);
        const auto right_seed = pipeline.seed(second);
        const auto left_mixed = pipeline.mix(left_seed, {10, 2, 0., 0.});
        const std::uint64_t right_lo = independent.second ? 3 : 2;
        const auto right_mixed = pipeline.mix(right_seed, {10, right_lo, 0., 0.});
        const auto left = mixed_reference(pipeline,
                                          pipeline.inspect(left_mixed),
                                          seeds(pipeline.inspect(left_seed), first),
                                          10,
                                          2,
                                          0.);
        const auto right = mixed_reference(pipeline,
                                           pipeline.inspect(right_mixed),
                                           seeds(pipeline.inspect(right_seed), second),
                                           10,
                                           right_lo,
                                           0.);
        Records combined;
        merge(combined, left);
        merge(combined, right);
        const auto stream =
            pipeline.linear_network({{left_mixed, 0}, {right_mixed, 1}}, {0, 1, 2}, 2, [](double) {
                return combiner(1., 1.);
            });
        check_records(pipeline.inspect(stream), combined);
        require(pipeline.inspect(stream).records.size() == 4,
                "independent RF or LO groups must remain distinct at equal mixed bins");
        scalar(pipeline.inspect(stream).reduction.total_power_w,
               4.L * std::norm(Wide{.13L, .02L}),
               "independent mixed groups sum their powers");
        const Parameters p;
        const auto expected = amplifier(combined, p, coefficients(p));
        const auto actual = pipeline.limited_amplifier(stream, model(p));
        check_result(pipeline, actual, expected, coefficients(p));
        require(actual.nonlinear_counts.physical_input_count == 4,
                "same-frequency independent mixed roots are separate nonlinear sources");
    }
}

void folded_parallel_branches_keep_local_histories() {
    CoherentPipeline pipeline(spacing, 50., 2);
    const std::vector<CoherentComponent> left_input{source(8, {.03, .004})};
    const std::vector<CoherentComponent> right_input{source(12, {.02, -.006})};
    const auto left_seed = pipeline.seed(left_input);
    const auto right_seed = pipeline.seed(right_input);
    const auto left_mix = pipeline.mix(left_seed, {10, 2, 0., .2});
    const auto right_mix = pipeline.mix(right_seed, {10, 2, 0., .2});
    auto left_expected = mixed_reference(pipeline,
                                         pipeline.inspect(left_mix),
                                         seeds(pipeline.inspect(left_seed), left_input),
                                         10,
                                         2,
                                         .2);
    auto right_expected = mixed_reference(pipeline,
                                          pipeline.inspect(right_mix),
                                          seeds(pipeline.inspect(right_seed), right_input),
                                          10,
                                          2,
                                          .2);
    const auto filter = [](double frequency) {
        return transfer(frequency == 2 * spacing ? 1. : 0.);
    };
    const auto left_if = pipeline.linear_network({{left_mix, 0}}, {0, 1}, 1, filter);
    const auto right_if = pipeline.linear_network({{right_mix, 0}}, {0, 1}, 1, filter);
    for (auto *records : {&left_expected, &right_expected}) {
        for (auto &record : *records) {
            if (record.first.bin != 2) {
                for (auto &term : record.second.history) {
                    term.second = Wide{};
                }
            }
        }
    }
    const auto &left_root = unique(pipeline.inspect(left_if), 2);
    const auto &right_root = unique(pipeline.inspect(right_if), 2);
    require(left_root.physical_identity.factors == right_root.physical_identity.factors &&
                left_root.history.front().factors != right_root.history.front().factors,
            "folded branches must share physical identity with different local source histories");
    const Parameters lp{8., 15., 19., 32., 23., 50.};
    const Parameters rp{3., 13., 17., 29., 21., 50.};
    const auto left_wanted = amplifier(left_expected, lp, coefficients(lp));
    const auto right_wanted = amplifier(right_expected, rp, coefficients(rp));
    const auto right = pipeline.limited_amplifier(right_if, model(rp));
    const auto left = pipeline.limited_amplifier(left_if, model(lp));
    check_result(pipeline, left, left_wanted, coefficients(lp));
    check_result(pipeline, right, right_wanted, coefficients(rp));
    const auto sum =
        pipeline.linear_network({{left.output, 0}, {right.output, 1}}, {0, 1, 2}, 2, [](double) {
            return combiner(1., 1.);
        });
    Records combined;
    merge(combined, left_wanted.records);
    merge(combined, right_wanted.records);
    check_records(pipeline.inspect(sum), combined);
    require(unique(pipeline.inspect(sum), 4, SpectrumKind::harmonic).history.size() == 2,
            "separately amplified branches have two square histories and no cross product");
    const auto next =
        pipeline.limited_amplifier(sum, model(lp), AmplifierInputMode::propagate_distortion);
    const auto next_expected = amplifier(combined, lp, coefficients(lp));
    check_result(pipeline, next, next_expected, coefficients(lp));
    require(unique(pipeline.inspect(next.output), 4, SpectrumKind::harmonic).history.size() == 3,
            "post-combiner amplification must add the new cross history to conducted squares");
    check_result(pipeline, left, left_wanted, coefficients(lp));
    check_result(pipeline, right, right_wanted, coefficients(rp));
}

void polynomial_mixer_amplifier_linear_polynomial_chain() {
    CoherentPipeline pipeline(spacing, 50., 2);
    const std::vector<CoherentComponent> inputs{source(8, {.13, -.04}, 1, .1)};
    const auto seed = pipeline.seed(inputs);
    const auto initial = seeds(pipeline.inspect(seed), inputs);
    const auto polynomial = pipeline.polynomial(seed, {0., 1., .2}, 2);
    Counts ignored;
    const auto before_mix = generate(initial, {0.L, 1.L, .2L}, 1.L, 50.L, 2, ignored, false);
    check_records(pipeline.inspect(polynomial.output), before_mix);
    const auto mixed = pipeline.mix(polynomial.output, {101, 2, -2., .43});
    const auto mixed_expected =
        mixed_reference(pipeline, pipeline.inspect(mixed), before_mix, 101, 2, .43, -2.);
    check_records(pipeline.inspect(mixed), mixed_expected);
    const auto &conjugated = unique(pipeline.inspect(mixed), 85, SpectrumKind::harmonic);
    require(conjugated.history.front().factors.size() == 3,
            "polynomial then mixer must keep both RF factors and the LO factor");
    std::size_t negative_rf = 0;
    for (const auto &factor : conjugated.history.front().factors) {
        if (pipeline.history_root(factor.root_id).role == RootRole::rf && factor.sign == -1) {
            ++negative_rf;
        }
    }
    require(negative_rf == 2, "low-side folding must conjugate the whole prior RF history");
    const Parameters p;
    const auto coefficient = coefficients(p);
    const auto amplified =
        pipeline.limited_amplifier(mixed, model(p), AmplifierInputMode::propagate_distortion);
    const auto amplified_expected = amplifier(mixed_expected, p, coefficient);
    check_result(pipeline, amplified, amplified_expected, coefficient);
    const auto &h2 = unique(pipeline.inspect(amplified.output), 186, SpectrumKind::harmonic);
    require(h2.physical_identity.factors.size() == 2 && h2.history.front().factors.size() == 4,
            "new harmonic classification must use mixed physical roots and retain RF/LO histories");
    const Complex gain{.37, -.21};
    const auto filtered =
        pipeline.linear_network({{amplified.output, 0}}, {0, 1}, 1, [&](double frequency) {
            return transfer(frequency == 93 * spacing || frequency == 186 * spacing ? gain
                                                                                    : Complex{});
        });
    Records filtered_expected = amplified_expected.records;
    for (auto &record : filtered_expected) {
        for (auto &term : record.second.history) {
            term.second *= record.first.bin == 93 || record.first.bin == 186 ? wide(gain) : Wide{};
        }
    }
    check_records(pipeline.inspect(filtered), filtered_expected);
    const auto final = pipeline.polynomial(filtered, {0., .8, -.16}, 12);
    Counts final_counts;
    const auto final_expected =
        generate(filtered_expected, {0.L, .8L, -.16L}, 1.L, 50.L, 12, final_counts, false);
    check_records(pipeline.inspect(final.output), final_expected);
    // The two direct source histories have order two; the two conducted
    // harmonics have order three. Filtering never changes their incident drive.
    const CoherentHighOrderAmplifier high(p.gain_db, p.p1_dbm, p.saturation_dbm, {.21, -.037}, 50.);
    const std::vector<long double> high_coefficient{0.L, 0.L, .21L, -.037L};
    for (std::size_t cutoff : {2U, 3U, 4U, 6U}) {
        const auto expected = amplifier(mixed_expected, p, high_coefficient, cutoff);
        const auto actual = pipeline.highorder_amplifier(
            mixed, high, cutoff, AmplifierInputMode::propagate_distortion);
        check_result(pipeline, actual, expected, high_coefficient);
        require(actual.direct_contribution_count == 4 &&
                    actual.discarded_direct_contributions == (cutoff == 2 ? 2U : 0U),
                "direct cutoff must use each history's complete source order");
        require(actual.nonlinear_counts.physical_input_count == 2,
                "mixed conducted harmonics must not generate secondary products");
    }
}

Folded folded_input(CoherentPipeline &pipeline, Complex a, Complex b, double phase) {
    const std::vector<CoherentComponent> inputs{source(8, a), source(12, b)};
    const auto seed = pipeline.seed(inputs);
    const auto initial = seeds(pipeline.inspect(seed), inputs);
    const auto mixed = pipeline.mix(seed, {10, 2, 0., phase});
    const auto &snapshot = pipeline.inspect(mixed);
    require(snapshot.records.size() == 3, "folded fixture must share one physical IF root");
    int lo = 0;
    for (const auto &record : snapshot.records) {
        for (const auto &term : record.history) {
            for (const auto &factor : term.factors) {
                const auto root = pipeline.history_root(factor.root_id);
                if (root.role == RootRole::lo && root.bin == 10 && root.coherence_group == 2) {
                    lo = static_cast<int>(root.id);
                }
            }
        }
    }
    require(lo != 0, "folded mixer must retain a named LO history root");
    const Wide pump = std::polar(1.L, static_cast<long double>(phase));
    Records expected;
    for (const auto &entry : initial) {
        for (bool difference : {true, false}) {
            const int bin = difference ? std::abs(entry.first.bin - 10) : entry.first.bin + 10;
            const auto &physical = unique(snapshot, bin);
            require(physical.physical_identity.factors.size() == 1,
                    "mixer must reset to a primitive physical root");
            require(
                pipeline.physical_root(physical.physical_identity.factors.front().root_id).role ==
                    RootRole::mixed,
                "mixed physical root role");
            auto &out =
                expected[{bin, SpectrumKind::source, labels(physical.physical_identity.factors)}];
            out.width = entry.second.width;
            for (const auto &term : entry.second.history) {
                const int sign = difference && entry.first.bin < 10 ? -1 : 1;
                const int lo_sign = difference && entry.first.bin > 10 ? -1 : 1;
                Factors history;
                append(history, term.first, sign);
                history.push_back(lo_sign * lo);
                out.history[canonical(std::move(history))] +=
                    (sign > 0 ? term.second : std::conj(term.second)) *
                    (lo_sign > 0 ? pump : std::conj(pump));
            }
        }
    }
    check_records(snapshot, expected);
    const auto filtered = pipeline.linear_network({{mixed, 0}}, {0, 1}, 1, [](double frequency) {
        return transfer(frequency == 2 * spacing ? 1. : 0.);
    });
    for (auto &entry : expected) {
        if (entry.first.bin != 2) {
            for (auto &term : entry.second.history) {
                term.second = Wide{};
            }
        }
    }
    check_records(pipeline.inspect(filtered), expected);
    return {filtered, expected};
}

void folded_if_histories_and_exact_cancellation() {
    const Parameters p;
    const auto coefficient = coefficients(p);
    for (bool cancelled : {false, true}) {
        CoherentPipeline pipeline(spacing, 50., 2);
        const auto folded = folded_input(pipeline,
                                         {.01, 0.},
                                         cancelled ? Complex{-.01, 0.} : Complex{.008, -.003},
                                         cancelled ? 0. : .27);
        const auto expected = amplifier(folded.expected, p, coefficient);
        const auto output = pipeline.limited_amplifier(folded.stream, model(p));
        check_result(pipeline, output, expected, coefficient);
        const auto &square = unique(pipeline.inspect(output.output), 4, SpectrumKind::harmonic);
        require(square.history.size() == 3, "folded IF square must retain AA, AB, BB histories");
        require(square.physical_identity.factors.size() == 2,
                "folded square must classify by its repeated physical mixed root");
        for (const auto &term : square.history) {
            require(term.factors.size() == 4, "nonlinear history must include RF and LO factors");
        }
        if (cancelled) {
            require(output.input_power_w == 0.,
                    "cancelled physical IF must have exactly zero drive");
            require(output.operating_point.nonlinear_input_scale == 1., "zero-drive limiter scale");
            require(square.component.amplitude == Complex{},
                    "cancelled quadratic aggregate must survive");
            const long double base = coefficient[2] * 5.L * .0001L;
            std::multiset<long double> expected_amplitudes{base, -2.L * base, base};
            for (const auto &term : square.history) {
                require(term.amplitude != Complex{},
                        "zero aggregate must keep nonzero generating paths");
                auto best = std::min_element(expected_amplitudes.begin(),
                                             expected_amplitudes.end(),
                                             [&](long double a, long double b) {
                                                 return std::abs(wide(term.amplitude) - a) <
                                                        std::abs(wide(term.amplitude) - b);
                                             });
                close(term.amplitude, *best, "explicit cancelling square binomial coefficient");
                expected_amplitudes.erase(best);
            }
            const auto again = pipeline.limited_amplifier(
                output.output, model(p), AmplifierInputMode::propagate_distortion);
            const auto next = amplifier(expected.records, p, coefficient);
            check_result(pipeline, again, next, coefficient);
            scalar(again.input_power_w, 0.L, "cancelled downstream common drive", 1e-30L);
        }
        const CoherentHighOrderAmplifier high(
            p.gain_db,
            p.p1_dbm,
            p.saturation_dbm,
            {static_cast<double>(coefficient[2]), static_cast<double>(coefficient[3])},
            50.);
        const auto high_result = pipeline.highorder_amplifier(folded.stream, high, 6);
        check_result(pipeline, high_result, expected, coefficient);
    }
}

void zero_monomials_distortion_only_and_mixed_drive() {
    CoherentPipeline pipeline(spacing, 50., 2);
    const Parameters p;
    const auto coefficient = coefficients(p);
    const std::vector<CoherentComponent> cancelled{source(7, {.04, -.02}), source(7, {-.04, .02})};
    const auto zero = pipeline.seed(cancelled);
    const auto zero_expected = amplifier(seeds(pipeline.inspect(zero), cancelled), p, coefficient);
    const auto silent = pipeline.limited_amplifier(zero, model(p));
    check_result(pipeline, silent, zero_expected, coefficient);
    require(silent.direct_contribution_count == 1 &&
                silent.nonlinear_counts.expanded_input_count == 1 &&
                silent.nonlinear_counts.generated_term_count == 0,
            "one normalized zero monomial must be retained but inactive for enumeration");
    const std::vector<CoherentComponent> inputs{source(7, {.2, -.03})};
    const auto seed = pipeline.seed(inputs);
    const auto original = seeds(pipeline.inspect(seed), inputs);
    for (bool with_source : {false, true}) {
        const auto poly = pipeline.polynomial(seed, {0., with_source ? 1. : 0., .7}, 2);
        Counts ignored;
        const auto expected_input =
            generate(original, {0.L, with_source ? 1.L : 0.L, .7L}, 1.L, 50.L, 2, ignored);
        check_records(pipeline.inspect(poly.output), expected_input);
        const auto expected = amplifier(expected_input, p, coefficient);
        const auto output = pipeline.limited_amplifier(
            poly.output, model(p), AmplifierInputMode::propagate_distortion);
        check_result(pipeline, output, expected, coefficient);
        if (!with_source) {
            require(
                output.input_power_w > 0. && output.nonlinear_counts.physical_input_count == 0 &&
                    output.nonlinear_counts.expanded_input_count == 0 &&
                    output.nonlinear_counts.generated_term_count == 0,
                "distortion-only input supplies drive and direct histories but no new products");
        }
        rejects<std::invalid_argument>([&] {
            pipeline.limited_amplifier(poly.output, model(p));
        });
        const auto silent_distortion =
            pipeline.linear_network({{poly.output, 0}}, {0, 1}, 1, [](double) {
                return transfer(0.);
            });
        rejects<std::invalid_argument>([&] {
            pipeline.limited_amplifier(silent_distortion, model(p));
        });
    }
}

void explicit_high_orders_and_cutoff() {
    const Parameters p{6., 12., 16., 32., 23., 75.};
    for (std::size_t degree : {4U, 7U, 9U, 11U}) {
        CoherentPipeline pipeline(spacing, 75., 1);
        const std::vector<CoherentComponent> inputs{source(7, {.12, -.035})};
        const auto seed = pipeline.seed(inputs);
        const auto initial = seeds(pipeline.inspect(seed), inputs);
        std::vector<double> nonlinear(degree - 1, 0.);
        nonlinear.back() = degree % 2 == 0 ? .043 : -.017;
        std::vector<long double> coefficient(degree + 1, 0.L);
        coefficient.back() = nonlinear.back();
        const CoherentHighOrderAmplifier high(
            p.gain_db, p.p1_dbm, p.saturation_dbm, nonlinear, 75.);
        for (std::size_t cutoff : {degree - 1, degree, degree + 1}) {
            const auto expected = amplifier(initial, p, coefficient, cutoff);
            const auto actual = pipeline.highorder_amplifier(seed, high, cutoff);
            check_result(pipeline, actual, expected, coefficient);
            require(actual.nonlinear_counts.generated_term_count == (degree + 1) / 2,
                    "single-tone local degree has hand-counted positive signed paths");
            if (cutoff < degree) {
                require(actual.nonlinear_counts.discarded_term_count == (degree + 1) / 2 &&
                            pipeline.inspect(actual.output).records.size() == 1,
                        "one-under cutoff must discard every degree-n history");
            }
        }
    }
}

void folded_cutoff_counts_lo_and_preserves_full_drive() {
    CoherentPipeline pipeline(spacing, 50., 2);
    const Parameters p;
    const auto folded = folded_input(pipeline, {.17, .02}, {.11, -.015}, .31);
    const std::vector<long double> coefficient{0.L, 0.L, .21L, -.037L};
    const CoherentHighOrderAmplifier high(p.gain_db, p.p1_dbm, p.saturation_dbm, {.21, -.037}, 50.);
    for (std::size_t cutoff : {1U, 2U, 3U, 4U, 5U, 6U}) {
        const auto expected = amplifier(folded.expected, p, coefficient, cutoff);
        const auto actual = pipeline.highorder_amplifier(folded.stream, high, cutoff);
        check_result(pipeline, actual, expected, coefficient);
        require(actual.input_power_w > .02 && actual.operating_point.nonlinear_input_scale < 1.,
                "uncut input, including discarded histories, must drive the common limiter");
        require(actual.direct_contribution_count == 4 &&
                    actual.nonlinear_counts.expanded_input_count == 4,
                "zero filtered records still contribute normalized input counts");
        require(actual.nonlinear_counts.generated_term_count == 13,
                "two active folded histories have 3 quadratic plus 10 cubic candidate paths");
        if (cutoff == 1) {
            require(actual.discarded_direct_contributions == 4 &&
                        pipeline.inspect(actual.output).records.empty(),
                    "LO factors can make every direct contribution exceed cutoff");
        } else if (cutoff < 4) {
            require(actual.nonlinear_counts.discarded_by_source_order ==
                        std::map<std::size_t, std::size_t>{{4, 3}, {6, 10}},
                    "discard histogram must count full RF-plus-LO order");
        } else if (cutoff < 6) {
            require(actual.nonlinear_counts.discarded_by_source_order ==
                        std::map<std::size_t, std::size_t>{{6, 10}},
                    "exact quadratic boundary retains order four, not cubic order six");
        }
    }
    // An empty nonlinear vector still carries the same full drive and direct history filtering.
    const CoherentHighOrderAmplifier linear(p.gain_db, p.p1_dbm, p.saturation_dbm, {}, 50.);
    const auto expected = amplifier(folded.expected, p, {0.L, 0.L}, 1);
    const auto actual = pipeline.highorder_amplifier(folded.stream, linear, 1);
    check_result(pipeline, actual, expected, {0.L, 0.L});
}

void empty_and_silent_highorder_inputs() {
    CoherentPipeline pipeline(spacing, 50., 1);
    const Parameters p;
    const auto empty = pipeline.seed({});
    const auto limited = pipeline.limited_amplifier(empty, model(p));
    check_result(pipeline, limited, amplifier({}, p, coefficients(p)), coefficients(p));
    const CoherentHighOrderAmplifier high(p.gain_db, p.p1_dbm, p.saturation_dbm, {.4, -.1}, 50.);
    const std::vector<long double> coefficient{0.L, 0.L, .4L, -.1L};
    const auto high_result = pipeline.highorder_amplifier(empty, high, 1);
    check_result(pipeline, high_result, amplifier({}, p, coefficient, 1), coefficient);
    const std::vector<CoherentComponent> inputs{source(7, {})};
    const auto seed = pipeline.seed(inputs);
    const auto actual = pipeline.highorder_amplifier(seed, high, 1);
    const auto expected = amplifier(seeds(pipeline.inspect(seed), inputs), p, coefficient, 1);
    check_result(pipeline, actual, expected, coefficient);
    require(actual.nonlinear_counts.expanded_input_count == 1 &&
                actual.nonlinear_counts.generated_term_count == 0 &&
                actual.direct_contribution_count == 1,
            "silent input is a known direct history, not an empty stream");
}
} // namespace

int main() {
    struct Case {
        const char *name;
        void (*run)();
    };

    const Case cases[]{
        {"source gain/limiter/reference sweep", source_only_gain_limiter_and_reference_sweep},
        {"independent groups and phases", independent_groups_and_two_tone_phases},
        {"two/three-stage physical cascade", two_and_three_stage_cascade},
        {"parallel branch identity reconciliation", parallel_branches_reuse_identities},
        {"independent RF/LO equal-bin classes", independent_rf_and_lo_at_equal_bins},
        {"folded parallel stream-local histories", folded_parallel_branches_keep_local_histories},
        {"polynomial/mixer/amplifier/linear/polynomial chain",
         polynomial_mixer_amplifier_linear_polynomial_chain},
        {"folded IF and cancelled nonlinear histories", folded_if_histories_and_exact_cancellation},
        {"silent monomials and distortion-only drive",
         zero_monomials_distortion_only_and_mixed_drive},
        {"explicit degrees 4/7/9/11", explicit_high_orders_and_cutoff},
        {"LO-aware cutoff, counters, and full drive",
         folded_cutoff_counts_lo_and_preserves_full_drive},
        {"empty and silent high-order streams", empty_and_silent_highorder_inputs}};
    for (const auto &test : cases) {
        try {
            test.run();
        } catch (const std::exception &error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
}
