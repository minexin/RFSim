#include "rfmodel/coherent/coherent_pipeline.hpp"
#include "../support/test_support.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace rfmodel;
using Wide = std::complex<long double>;
using Spectrum = std::map<int, Wide>;
constexpr double spacing = 1e8;
constexpr double reference = 50.;
const Complex a{.01, .003};
const Complex b{.02, -.002};

Wide wide(Complex x) {
    return {x.real(), x.imag()};
}

void close(Complex actual,
           Wide expected,
           const char *message,
           long double absolute = 1e-23L,
           long double relative = 2e-11L) {
    require(std::abs(wide(actual) - expected) <= absolute + relative * std::abs(expected), message);
}

void scalar(double actual,
            long double expected,
            const char *message,
            long double absolute = 1e-23L,
            long double relative = 2e-12L) {
    require(std::abs(static_cast<long double>(actual) - expected) <=
                absolute + relative * std::abs(expected),
            message);
}

// Independent oracle: ordered convolution of a signed voltage Fourier spectrum.
// It deliberately does not call polynomial, origin, reduction, or pipeline kernels.
Spectrum convolve(const Spectrum &left, const Spectrum &right) {
    Spectrum result;
    for (const auto &x : left) {
        for (const auto &y : right) {
            result[x.first + y.first] += x.second * y.second;
        }
    }
    return result;
}

Spectrum polynomial_reference(const Spectrum &rf, const std::vector<double> &coefficients) {
    const long double scale = std::sqrt(static_cast<long double>(reference) / 2.L);
    Spectrum voltage;
    for (const auto &entry : rf) {
        require(entry.first > 0, "oracle input must be RF projected");
        voltage[entry.first] += scale * entry.second;
        voltage[-entry.first] += scale * std::conj(entry.second);
    }
    Spectrum result;
    Spectrum power{{0, Wide{1.L, 0.L}}};
    for (std::size_t degree = 0; degree < coefficients.size(); ++degree) {
        if (degree != 0) {
            power = convolve(power, voltage);
        }
        if (coefficients[degree] == 0.) {
            continue;
        }
        for (const auto &entry : power) {
            if (entry.first > 0) {
                result[entry.first] +=
                    static_cast<long double>(coefficients[degree]) * entry.second / scale;
            }
        }
    }
    // DC is intentionally absent before any subsequent stage.
    return result;
}

Spectrum mixer_reference(const Spectrum &rf, int lo, long double phase, long double gain_db) {
    const Wide pump = std::polar(1.L, phase);
    const long double gain = std::pow(10.L, gain_db / 20.L);
    Spectrum result;
    for (const auto &entry : rf) {
        require(entry.first != lo, "oracle fixture must avoid DC mixing");
        result[std::abs(entry.first - lo)] +=
            gain *
            (entry.first > lo ? entry.second * std::conj(pump) : std::conj(entry.second) * pump);
        result[entry.first + lo] += gain * entry.second * pump;
    }
    return result;
}

Spectrum aggregate(const StreamSnapshot &snapshot) {
    Spectrum result;
    for (const auto &record : snapshot.records) {
        result[record.component.bin] += wide(record.component.amplitude);
    }
    return result;
}

void check_spectrum(const StreamSnapshot &snapshot,
                    const Spectrum &expected,
                    const char *message,
                    long double absolute = 1e-23L) {
    const auto actual = aggregate(snapshot);
    for (const auto &entry : expected) {
        const auto found = actual.find(entry.first);
        require(found != actual.end(), "expected positive RF bin missing");
        require(std::abs(found->second - entry.second) <=
                    absolute + 2e-10L * std::abs(entry.second),
                message);
    }
    for (const auto &entry : actual) {
        const auto found = expected.find(entry.first);
        if (found == expected.end()) {
            require(std::abs(entry.second) <= absolute, "unexpected nonzero positive RF bin");
        }
    }
}

CoherentComponent
source(int bin, Complex amplitude, std::uint64_t group = 1, double bandwidth = 1.) {
    return {bin, SpectrumKind::source, bandwidth, group, amplitude};
}

CoherentPipeline::Stream seed_pair(CoherentPipeline &pipeline,
                                   Complex first = a,
                                   Complex second = b,
                                   std::uint64_t second_group = 1,
                                   double bandwidth = 1.) {
    return pipeline.seed(
        {source(8, first, 1, bandwidth), source(12, second, second_group, bandwidth)});
}

MixerDrive drive(int bin = 10, std::uint64_t group = 2, double gain = 0., double phase = .3) {
    return {bin, group, gain, phase};
}

LinearNetwork transfer(Complex gain, double ohms = reference) {
    LinearNetwork network(ohms);
    network.add({2, {0., 0., gain, 0.}});
    return network;
}

LinearNetwork combiner(Complex left, Complex right) {
    LinearNetwork network;
    network.add({3, {0., 0., 0., 0., 0., 0., left, right, 0.}});
    return network;
}

const StreamRecord &
unique_record(const StreamSnapshot &snapshot, int bin, SpectrumKind kind = SpectrumKind::source) {
    const StreamRecord *result = nullptr;
    for (const auto &record : snapshot.records) {
        if (record.component.bin == bin && record.component.kind == kind) {
            require(result == nullptr, "fixture expected one physical component");
            result = &record;
        }
    }
    require(result != nullptr, "fixture physical component missing");
    return *result;
}

MixingOrigin canonical(MixingOrigin factors) {
    std::sort(factors.begin(), factors.end(), [](const auto &left, const auto &right) {
        return std::tie(left.root_id, left.sign) < std::tie(right.root_id, right.sign);
    });
    return factors;
}

const OriginContribution &term_with(const StreamRecord &record, const MixingOrigin &factors) {
    const auto expected = canonical(factors);
    const OriginContribution *found = nullptr;
    for (const auto &term : record.history) {
        if (term.factors == expected) {
            require(found == nullptr, "duplicate canonical history monomial");
            found = &term;
        }
    }
    require(found != nullptr, "explicit expected history monomial missing");
    return *found;
}

std::uint64_t history_root_id(const CoherentPipeline &pipeline,
                              const StreamSnapshot &snapshot,
                              RootRole role,
                              std::uint64_t group,
                              int bin) {
    std::uint64_t result = 0;
    for (const auto &record : snapshot.records) {
        for (const auto &term : record.history) {
            for (const auto &factor : term.factors) {
                const auto root = pipeline.history_root(factor.root_id);
                if (root.role == role && root.coherence_group == group && root.bin == bin) {
                    require(result == 0 || result == factor.root_id,
                            "same source metadata allocated duplicate history roots");
                    result = factor.root_id;
                }
            }
        }
    }
    require(result != 0, "expected history root not found");
    return result;
}

void check_support(const CoherentPipeline &pipeline, const StreamSnapshot &snapshot) {
    for (const auto &record : snapshot.records) {
        Wide amplitude{};
        for (const auto &term : record.history) {
            require(!term.factors.empty(), "history factors must not be empty");
            require(canonical(term.factors) == term.factors, "history factors not canonical");
            std::int64_t frequency = 0;
            long double bandwidth = 0.;
            for (const auto &factor : term.factors) {
                require(factor.sign == -1 || factor.sign == 1, "invalid history sign");
                const auto root = pipeline.history_root(factor.root_id);
                require(root.id == factor.root_id, "history root id mismatch");
                require(root.role != RootRole::mixed,
                        "history was replaced by mixed physical root");
                frequency += static_cast<std::int64_t>(factor.sign) * root.bin;
                bandwidth += root.bandwidth_hz;
                if (root.role == RootRole::lo) {
                    require(root.bandwidth_hz == 0., "LO history bandwidth must be zero");
                }
            }
            require(frequency == record.component.bin, "signed history frequency mismatch");
            scalar(record.component.bandwidth_hz, bandwidth, "unsigned history bandwidth mismatch");
            amplitude += wide(term.amplitude);
        }
        close(record.component.amplitude,
              amplitude,
              "history contributions differ from component",
              2e-18L);
        require(canonical(record.physical_identity.factors) == record.physical_identity.factors,
                "physical factors not canonical");
        std::int64_t frequency = 0;
        long double bandwidth = 0.;
        for (const auto &factor : record.physical_identity.factors) {
            const auto root = pipeline.physical_root(factor.root_id);
            require(root.id == factor.root_id && root.role != RootRole::lo,
                    "invalid physical root metadata");
            frequency += static_cast<std::int64_t>(factor.sign) * root.bin;
            bandwidth += root.bandwidth_hz;
        }
        require(frequency == record.component.bin, "signed physical frequency mismatch");
        scalar(record.component.bandwidth_hz, bandwidth, "unsigned physical bandwidth mismatch");
    }
}

void mixed_polynomial_matches_convolution() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto input = seed_pair(pipeline);
    const auto mixed = pipeline.mix(input, drive());
    const auto &snapshot = pipeline.inspect(mixed);
    require(snapshot.records.size() == 3, "folded mixer should have three physical components");
    const auto &folded = unique_record(snapshot, 2);
    require(folded.history.size() == 2, "folded IF must retain two source histories");
    const Wide pump = std::polar(1.L, .3L);
    close(folded.component.amplitude,
          std::conj(wide(a)) * pump + wide(b) * std::conj(pump),
          "folded IF complex wave");
    close(unique_record(snapshot, 18).component.amplitude, wide(a) * pump, "lower sum wave");
    close(unique_record(snapshot, 22).component.amplitude, wide(b) * pump, "upper sum wave");
    require(folded.physical_identity.factors.size() == 1, "mixer must rebase physical identity");
    const auto physical = pipeline.physical_root(folded.physical_identity.factors.front().root_id);
    require(physical.role == RootRole::mixed && physical.bin == 2 && physical.bandwidth_hz == 1.,
            "mixed physical root metadata");
    const auto output = pipeline.polynomial(mixed, {0., 1., .5}, 4);
    const auto expected = polynomial_reference(
        mixer_reference({{8, wide(a)}, {12, wide(b)}}, 10, .3L, 0.L), {0., 1., .5});
    check_spectrum(
        pipeline.inspect(output.output), expected, "mixed polynomial independent oracle");
    check_support(pipeline, snapshot);
    check_support(pipeline, pipeline.inspect(output.output));
}

void cancelled_histories_survive_recursion() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto input = seed_pair(pipeline, .01, -.01);
    const auto mixed = pipeline.mix(input, drive(10, 2, 0., 0.));
    const auto &initial = pipeline.inspect(mixed);
    const auto &folded = unique_record(initial, 2);
    require(folded.component.amplitude == Complex{}, "IF cancellation should be exact");
    require(folded.history.size() == 2, "cancelled IF history erased");
    for (const auto &term : folded.history) {
        require(term.amplitude != Complex{}, "cancelled IF lost nonzero history");
    }
    const auto rf8 = history_root_id(pipeline, initial, RootRole::rf, 1, 8);
    const auto rf12 = history_root_id(pipeline, initial, RootRole::rf, 1, 12);
    const auto lo = history_root_id(pipeline, initial, RootRole::lo, 2, 10);
    const MixingOrigin aa{{rf8, -1}, {rf8, -1}, {lo, 1}, {lo, 1}};
    const MixingOrigin ab{{rf8, -1}, {rf12, 1}, {lo, -1}, {lo, 1}};
    const MixingOrigin bb{{rf12, 1}, {rf12, 1}, {lo, -1}, {lo, -1}};
    const auto first = pipeline.polynomial(mixed, {0., 1., .5}, 4);
    const auto &harmonic = unique_record(pipeline.inspect(first.output), 4, SpectrumKind::harmonic);
    require(harmonic.history.size() == 3, "cancelled square must retain three monomials");
    close(term_with(harmonic, aa).amplitude, .00025L, "cancelled A squared coefficient");
    close(term_with(harmonic, ab).amplitude, -.0005L, "cancelled cross multiplicity");
    close(term_with(harmonic, bb).amplitude, .00025L, "cancelled B squared coefficient");
    require(harmonic.component.amplitude == Complex{}, "cancelled square aggregate must be zero");
    for (const auto &term : harmonic.history) {
        require(term.factors.size() == 4, "square source order must include both LO factors");
        std::size_t rf_count = 0;
        for (const auto &factor : term.factors) {
            rf_count += pipeline.history_root(factor.root_id).role == RootRole::rf ? 1U : 0U;
        }
        require(rf_count == 2, "cancelled square RF order");
    }
    // Isolate the cancelled IF physically; non-IF zero identities may remain.
    const auto filtered = pipeline.linear_network({{mixed, 0}}, {0, 1}, 1, [](double frequency) {
        return transfer(frequency == 2 * spacing ? 1. : 0.);
    });
    const auto square = pipeline.polynomial(filtered, {0., 0., .5}, 4);
    const auto fourth = pipeline.polynomial(square.output, {0., 0., .5}, 8);
    const auto &continued =
        unique_record(pipeline.inspect(fourth.output), 8, SpectrumKind::harmonic);
    require(continued.history.size() == 5, "second nonlinear stage lost cancelled histories");
    const long double coefficients[]{1.L, -4.L, 6.L, -4.L, 1.L};
    for (std::size_t count_b = 0; count_b <= 4; ++count_b) {
        MixingOrigin factors;
        for (std::size_t j = count_b; j < 4; ++j) {
            factors.push_back({rf8, -1});
            factors.push_back({lo, 1});
        }
        for (std::size_t j = 0; j < count_b; ++j) {
            factors.push_back({rf12, 1});
            factors.push_back({lo, -1});
        }
        close(term_with(continued, factors).amplitude,
              1.5625e-7L * coefficients[count_b],
              "fourth-order cancelled binomial coefficient");
    }
    close(continued.component.amplitude, Wide{}, "recursive cancellation residual", 2e-21L);
    check_support(pipeline, pipeline.inspect(first.output));
    check_support(pipeline, pipeline.inspect(fourth.output));
}

void stream_locality_and_complex_combiner() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto left = pipeline.mix(pipeline.seed({source(8, a)}), drive());
    const auto right = pipeline.mix(pipeline.seed({source(12, b)}), drive());
    const auto &l = unique_record(pipeline.inspect(left), 2);
    const auto &r = unique_record(pipeline.inspect(right), 2);
    require(l.component.coherence_group == r.component.coherence_group,
            "persistent RF/LO pair must share physical group across mix calls");
    require(l.physical_identity.factors == r.physical_identity.factors,
            "folded IF identity must be shared across independent stream snapshots");
    require(l.history.size() == 1 && r.history.size() == 1 &&
                l.history.front().factors != r.history.front().factors,
            "same physical identity must not overwrite stream-local history");
    const auto before_left = l.history;
    const auto before_right = r.history;
    std::map<double, std::size_t> calls;
    const auto combined =
        pipeline.linear_network({{left, 0}, {right, 1}}, {2, 0, 1}, 2, [&](double frequency) {
            ++calls[frequency];
            return combiner(.5, Complex{0., .5});
        });
    const auto &c = unique_record(pipeline.inspect(combined), 2);
    const Wide pump = std::polar(1.L, .3L);
    const Wide expected =
        .5L * std::conj(wide(a)) * pump + Wide{0.L, .5L} * wide(b) * std::conj(pump);
    close(c.component.amplitude, expected, "complex recombination wave");
    scalar(pipeline.inspect(combined).reduction.power_by_bin_w.at(2),
           std::norm(expected),
           "complex recombination physical power");
    require(c.history.size() == 2, "recombination must retain both stream-local histories");
    require(c.component.coherence_group == l.component.coherence_group,
            "temporary network groups leaked into restored identity");
    require(calls == std::map<double, std::size_t>{{2 * spacing, 1},
                                                   {18 * spacing, 1},
                                                   {22 * spacing, 1}},
            "network must build once for each distinct actual frequency");
    require(l.history.front().factors == before_left.front().factors &&
                l.history.front().amplitude == before_left.front().amplitude &&
                r.history.front().factors == before_right.front().factors &&
                r.history.front().amplitude == before_right.front().amplitude,
            "new output mutated stable input snapshots");
    check_support(pipeline, pipeline.inspect(combined));
}

void repeated_mixer_conjugates_all_history() {
    CoherentPipeline pipeline(spacing, reference, 3);
    const auto mixed = pipeline.mix(seed_pair(pipeline), drive());
    const auto polynomial = pipeline.polynomial(mixed, {0., 1., .5}, 4);
    const auto &before =
        unique_record(pipeline.inspect(polynomial.output), 4, SpectrumKind::harmonic);
    const auto again = pipeline.mix(polynomial.output, drive(101, 3, -2., -.4));
    const auto &after = unique_record(pipeline.inspect(again), 97, SpectrumKind::harmonic);
    const auto &sum = unique_record(pipeline.inspect(again), 105, SpectrumKind::harmonic);
    const auto lo101 = history_root_id(pipeline, pipeline.inspect(again), RootRole::lo, 3, 101);
    const Wide pump = std::polar(1.L, -.4L);
    const long double gain = std::pow(10.L, -.1L);
    require(after.history.size() == before.history.size(),
            "conjugating mixer changed monomial count");
    for (const auto &old : before.history) {
        MixingOrigin difference_factors;
        MixingOrigin sum_factors = old.factors;
        for (const auto &factor : old.factors) {
            difference_factors.push_back({factor.root_id, -factor.sign});
        }
        difference_factors.push_back({lo101, 1});
        sum_factors.push_back({lo101, 1});
        const auto &difference = term_with(after, difference_factors);
        close(difference.amplitude,
              gain * std::conj(wide(old.amplitude)) * pump,
              "mixer must conjugate both amplitude and full prior history");
        close(term_with(sum, sum_factors).amplitude,
              gain * wide(old.amplitude) * pump,
              "sum mixing must preserve old signs");
        require(difference.factors.size() == 5, "repeated mixer source order must grow by one");
    }
    require(after.component.kind == before.component.kind &&
                after.physical_identity.factors.size() == 1,
            "mixer must preserve local kind and rebase physical identity");
    const auto new_root = pipeline.physical_root(after.physical_identity.factors.front().root_id);
    require(new_root.role == RootRole::mixed && new_root.bin == 97,
            "new mixer physical primitive metadata");
    check_spectrum(pipeline.inspect(again),
                   mixer_reference(polynomial_reference(
                                       mixer_reference({{8, wide(a)}, {12, wide(b)}}, 10, .3L, 0.L),
                                       {0., 1., .5}),
                                   101,
                                   -.4L,
                                   -2.L),
                   "repeated mixer whole-spectrum direct shift");
    check_support(pipeline, pipeline.inspect(again));
}

void polynomial_mixer_polynomial_chain() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto input = seed_pair(pipeline);
    const auto pre = pipeline.polynomial(input, {0., 1., .1}, 2);
    const auto mixed = pipeline.mix(pre.output, drive(101));
    const auto post = pipeline.polynomial(mixed, {0., 1., .5}, 6);
    auto expected = polynomial_reference({{8, wide(a)}, {12, wide(b)}}, {0., 1., .1});
    expected = mixer_reference(expected, 101, .3L, 0.L);
    expected = polynomial_reference(expected, {0., 1., .5});
    require(pre.counts.discarded_term_count == 0 && post.counts.discarded_term_count == 0,
            "order-six independent chain must not discard paths");
    check_spectrum(pipeline.inspect(post.output),
                   expected,
                   "polynomial mixer polynomial independent convolution chain");
    check_support(pipeline, pipeline.inspect(post.output));
}

void hand_counted_source_order_cutoffs() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto mixed = pipeline.mix(seed_pair(pipeline), drive());
    for (const std::size_t cutoff : {3U, 1U}) {
        const auto result = pipeline.polynomial(mixed, {0., 1., .5}, cutoff);
        require(result.counts.physical_input_count == 3 && result.counts.expanded_input_count == 4,
                "mixed polynomial physical/expanded input counts");
        require(result.counts.generated_term_count == 19,
                "four mixed contributions generate 4 linear and 15 quadratic paths");
        if (cutoff == 3) {
            require(result.counts.discarded_term_count == 15 &&
                        result.counts.discarded_by_source_order ==
                            std::map<std::size_t, std::size_t>{{4, 15}},
                    "cutoff three must remove exactly the fifteen source-order-four paths");
            check_spectrum(pipeline.inspect(result.output),
                           mixer_reference({{8, wide(a)}, {12, wide(b)}}, 10, .3L, 0.L),
                           "cutoff three must retain original mixed RF spectrum");
        } else {
            require(result.counts.discarded_term_count == 19 &&
                        result.counts.discarded_by_source_order ==
                            std::map<std::size_t, std::size_t>{{2, 4}, {4, 15}},
                    "cutoff one must remove all mixed source orders, including LO factors");
            require(pipeline.inspect(result.output).records.empty(), "cutoff one should be empty");
        }
    }

    CoherentPipeline recursive(spacing, reference, 1);
    const auto input = recursive.seed({source(10, .01)});
    const auto first = recursive.polynomial(input, {0., 1., .5}, 4);
    for (const std::size_t cutoff : {3U, 2U, 1U}) {
        const auto result = recursive.polynomial(first.output, {0., 1., .5}, cutoff);
        require(result.counts.generated_term_count == 6, "one-tone recursion has six native paths");
        const std::size_t discarded = cutoff == 3 ? 1 : cutoff == 2 ? 3 : 5;
        const std::size_t retained = cutoff == 3 ? 4 : cutoff == 2 ? 2 : 1;
        require(result.counts.discarded_term_count == discarded &&
                    recursive.inspect(result.output).records.size() == retained,
                "hand-counted recursive source-order cutoff");
        const std::map<std::size_t, std::size_t> by_order =
            cutoff == 3   ? std::map<std::size_t, std::size_t>{{4, 1}}
            : cutoff == 2 ? std::map<std::size_t, std::size_t>{{3, 2}, {4, 1}}
                          : std::map<std::size_t, std::size_t>{{2, 2}, {3, 2}, {4, 1}};
        require(result.counts.discarded_by_source_order == by_order,
                "recursive discarded counts by original source order");
    }
    const auto second = recursive.polynomial(first.output, {0., 1., .5}, 4);
    const auto &snapshot = recursive.inspect(second.output);
    require(snapshot.records.size() == 5, "one-tone full recursion physical identities");
    close(unique_record(snapshot, 20, SpectrumKind::harmonic).component.amplitude,
          .0005L,
          "recursive second harmonic coherent alias");
    close(unique_record(snapshot, 30, SpectrumKind::harmonic).component.amplitude,
          .0000125L,
          "recursive third harmonic");
    close(unique_record(snapshot, 40, SpectrumKind::harmonic).component.amplitude,
          .00000015625L,
          "recursive fourth harmonic",
          1e-25L);
    const auto &difference = unique_record(snapshot, 10, SpectrumKind::intermod);
    require(difference.history.size() == 1 && difference.history.front().factors.size() == 3,
            "recursive difference must retain third-order history");
    const auto root = history_root_id(recursive, snapshot, RootRole::rf, 1, 10);
    require(difference.history.front().factors == canonical({{root, -1}, {root, 1}, {root, 1}}),
            "opposite root factors must not cancel");
    const auto linear = recursive.polynomial(second.output, {0., 1.}, 4);
    const auto &propagated = recursive.inspect(linear.output);
    require(propagated.records.size() == snapshot.records.size(), "linear stage lost identities");
    for (const auto &record : snapshot.records) {
        const auto &new_record =
            unique_record(propagated, record.component.bin, record.component.kind);
        require(new_record.physical_identity.factors == record.physical_identity.factors &&
                    new_record.history.front().factors == record.history.front().factors,
                "local order one must preserve global history and physical identity");
        close(new_record.component.amplitude,
              wide(record.component.amplitude),
              "locally linear stage changed amplitude");
    }
    const auto low_linear = recursive.polynomial(second.output, {0., 1.}, 1);
    require(low_linear.counts.generated_term_count == 5 &&
                low_linear.counts.discarded_term_count == 4,
            "locally linear source cutoff must retain original global source order");
    check_support(recursive, snapshot);
}

long double compression_gain_reference(long double power) {
    const long double gain = 10.L;
    const long double compression = std::pow(10.L, -.05L);
    const long double input_p1db = std::pow(10.L, -2.9L);
    if (power <= input_p1db) {
        return gain * (1.L - (1.L - compression) * power / input_p1db);
    }
    const long double output_anchor = std::sqrt(.1L);
    const long double headroom = std::sqrt(std::pow(10.L, -.7L)) - output_anchor;
    const long double drive_amplitude = std::sqrt(power);
    const long double slope = gain * (3.L * compression - 2.L);
    const long double output =
        output_anchor +
        headroom * std::tanh(slope * (drive_amplitude - std::sqrt(input_p1db)) / headroom);
    return output / drive_amplitude;
}

void shared_physical_drive_and_compression() {
    const SaturatingFundamentalCompression model(20., 20., 23.);
    for (const double scale : {.2, 1., 4.}) {
        CoherentPipeline pipeline(spacing, reference, 2);
        const auto mixed = pipeline.mix(seed_pair(pipeline, scale * a, scale * b), drive());
        const auto &before = pipeline.inspect(mixed);
        const Wide pump = std::polar(1.L, .3L);
        const Wide left = static_cast<long double>(scale) * wide(a);
        const Wide right = static_cast<long double>(scale) * wide(b);
        const long double power = std::norm(std::conj(left) * pump + right * std::conj(pump)) +
                                  std::norm(left) + std::norm(right);
        const long double naive_power = 2.L * (std::norm(left) + std::norm(right));
        require(std::abs(power - naive_power) > .1L * naive_power,
                "shared-drive fixture must distinguish physical and flattened power");
        const auto compressed = pipeline.compress_fundamentals(mixed, model);
        scalar(compressed.input_power_w, power, "compression must use restored physical drive");
        const long double gain = compression_gain_reference(power);
        scalar(compressed.common_amplitude_gain, gain, "independent piecewise compression gain");
        const auto &after = pipeline.inspect(compressed.output);
        require(after.records.size() == before.records.size(), "compression changed identities");
        for (const auto &record : before.records) {
            const auto &result = unique_record(after, record.component.bin, record.component.kind);
            require(result.component.coherence_group == record.component.coherence_group &&
                        result.physical_identity.factors == record.physical_identity.factors &&
                        result.history.size() == record.history.size(),
                    "compression changed physical/history structure");
            for (const auto &term : record.history) {
                close(term_with(result, term.factors).amplitude,
                      gain * wide(term.amplitude),
                      "every retained history must receive the same scalar gain");
            }
        }
        scalar(
            after.reduction.total_power_w, power * gain * gain, "compressed total physical power");
        check_support(pipeline, after);
    }
}

void filtered_cancellation_zero_drive() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto mixed = pipeline.mix(seed_pair(pipeline, .01, -.01), drive(10, 2, 0., 0.));
    std::map<double, std::size_t> calls;
    const auto filtered = pipeline.linear_network({{mixed, 0}}, {0, 1}, 1, [&](double frequency) {
        ++calls[frequency];
        return transfer(frequency == 2 * spacing ? 1. : 0.);
    });
    require(pipeline.inspect(filtered).reduction.total_power_w == 0., "IF-only cancellation drive");
    const auto compressed =
        pipeline.compress_fundamentals(filtered, SaturatingFundamentalCompression(20., 20., 23.));
    require(compressed.input_power_w == 0. && compressed.common_amplitude_gain == 10.,
            "zero-drive compression must use finite small-signal gain");
    const auto &folded = unique_record(pipeline.inspect(compressed.output), 2);
    require(folded.history.size() == 2 && folded.component.amplitude == Complex{},
            "zero-drive compression must retain cancelling histories");
    bool positive = false, negative = false;
    for (const auto &term : folded.history) {
        if (term.amplitude.real() > 0.) {
            close(term.amplitude, .1L, "zero-drive positive retained history");
            positive = true;
        } else {
            close(term.amplitude, -.1L, "zero-drive negative retained history");
            negative = true;
        }
    }
    require(positive && negative, "zero-drive histories must have both cancelling signs");
    require(pipeline.inspect(compressed.output).reduction.total_power_w == 0.,
            "retained filtered zero identities must not add power");
    require(calls == std::map<double, std::size_t>{{2 * spacing, 1},
                                                   {18 * spacing, 1},
                                                   {22 * spacing, 1}},
            "filter extraction count includes cancelled and noncancelled bins");
    check_support(pipeline, pipeline.inspect(compressed.output));
}

void independent_sources_and_lo_groups() {
    CoherentPipeline pipeline(spacing, reference, 3);
    const auto mixed = pipeline.mix(seed_pair(pipeline, a, b, 3), drive());
    const auto &snapshot = pipeline.inspect(mixed);
    std::set<std::uint64_t> groups;
    for (const auto &record : snapshot.records) {
        if (record.component.bin == 2) {
            groups.insert(record.component.coherence_group);
            require(record.history.size() == 1, "independent IF histories falsely merged");
        }
    }
    require(groups.size() == 2 && snapshot.records.size() == 4,
            "independent RF groups must remain separate at folded IF");
    const long double independent_power = std::norm(wide(a)) + std::norm(wide(b));
    scalar(snapshot.reduction.power_by_bin_w.at(2), independent_power, "independent IF power sum");
    scalar(
        snapshot.reduction.total_power_w, 2.L * independent_power, "independent mixer total drive");
    const auto compressed =
        pipeline.compress_fundamentals(mixed, SaturatingFundamentalCompression(20., 20., 23.));
    scalar(compressed.input_power_w,
           2.L * independent_power,
           "temporary expanded groups must not determine compression power");

    CoherentPipeline separate_lo(spacing, reference, 3);
    const auto first = separate_lo.mix(separate_lo.seed({source(8, .01)}), drive(10, 2, 0., 0.));
    const auto second = separate_lo.mix(separate_lo.seed({source(12, -.01)}), drive(10, 3, 0., 0.));
    require(unique_record(separate_lo.inspect(first), 2).component.coherence_group !=
                unique_record(separate_lo.inspect(second), 2).component.coherence_group,
            "different LO groups must allocate different physical mixing identities");
    const auto combined =
        separate_lo.linear_network({{first, 0}, {second, 1}}, {0, 1, 2}, 2, [](double) {
            return combiner(1., 1.);
        });
    const auto &result = separate_lo.inspect(combined);
    scalar(result.reduction.power_by_bin_w.at(2),
           .0002L,
           "separate LO groups must prevent false coherent cancellation");
    scalar(result.reduction.total_power_w, .0004L, "separate LO total power");
    require(std::abs(aggregate(result).at(2)) < 1e-22L, "frequency-only sum deliberately cancels");
    check_support(pipeline, snapshot);
    check_support(separate_lo, result);
}

void fractional_support_and_compensated_sum() {
    CoherentPipeline pipeline(spacing, reference, 1);
    const auto input = pipeline.seed({source(10, Complex{.01, .003}, 1, .1)});
    const auto first = pipeline.polynomial(input, {0., 1., .5, -.2}, 9);
    const auto second = pipeline.polynomial(first.output, {0., 1., .5, -.2}, 9);
    const auto &snapshot = pipeline.inspect(second.output);
    using Identity = std::tuple<int, SpectrumKind, std::vector<std::pair<std::uint64_t, int>>>;
    std::set<Identity> identities;
    bool opposite = false;
    bool repeated = false;
    for (const auto &record : snapshot.records) {
        require(record.history.size() == 1,
                "one-source recursively identical histories should canonicalize to one monomial");
        const auto &term = record.history.front();
        std::vector<std::pair<std::uint64_t, int>> factors;
        std::size_t negative = 0, positive = 0;
        for (const auto &factor : term.factors) {
            factors.emplace_back(factor.root_id, factor.sign);
            negative += factor.sign < 0 ? 1U : 0U;
            positive += factor.sign > 0 ? 1U : 0U;
        }
        require(identities.emplace(record.component.bin, record.component.kind, factors).second,
                "fractional widths created duplicate parenthesized identity");
        const double expected_width = static_cast<double>(
            static_cast<long double>(.1) * static_cast<long double>(term.factors.size()));
        require(record.component.bandwidth_hz == expected_width,
                "fractional support width must be canonical root accumulation");
        opposite = opposite || (negative != 0 && positive != 0);
        repeated = repeated || positive > 1 || negative > 1;
    }
    require(opposite && repeated, "fractional fixture must exercise repeats and opposite factors");
    auto expected = polynomial_reference({{10, wide(Complex{.01, .003})}}, {0., 1., .5, -.2});
    expected = polynomial_reference(expected, {0., 1., .5, -.2});
    check_spectrum(
        snapshot, expected, "fractional recursive independent polynomial spectrum", 1e-25L);
    check_support(pipeline, snapshot);

    // Neumaier-style compensated aggregation must retain the small middle term.
    // No production expression or coherent reducer is used to derive this result.
    CoherentPipeline cancellation(spacing, reference, 1);
    const auto large = cancellation.seed({source(10, Complex{1e16, 1e16})});
    const auto small = cancellation.seed({source(10, Complex{1., 2.})});
    const auto negative = cancellation.seed({source(10, Complex{-1e16, -1e16})});
    const auto combined =
        cancellation.linear_network({{large, 0}, {small, 0}, {negative, 0}}, {0, 1}, 1, [](double) {
            return transfer(1.);
        });
    const auto &result = unique_record(cancellation.inspect(combined), 10);
    require(result.history.size() == 1 && result.component.amplitude == Complex(1., 2.) &&
                result.history.front().amplitude == Complex(1., 2.),
            "compensated identical-monomial sum lost small residual");
    require(cancellation.inspect(combined).reduction.total_power_w == 5.,
            "compensated physical power residual");
    const auto zero =
        cancellation.linear_network({{large, 0}, {negative, 0}}, {0, 1}, 1, [](double) {
            return transfer(1.);
        });
    const auto &known_zero = unique_record(cancellation.inspect(zero), 10);
    require(known_zero.history.size() == 1 && known_zero.history.front().amplitude == Complex{} &&
                known_zero.component.amplitude == Complex{},
            "identical monomials must merge to a retained known zero history");
}

void generated_frequencies_and_empty_network_validation() {
    CoherentPipeline pipeline(spacing, reference, 2);
    const auto input = seed_pair(pipeline);
    const auto poly = pipeline.polynomial(input, {0., 1., .5}, 2);
    const auto &before = pipeline.inspect(poly.output);
    std::map<double, std::size_t> calls;
    const auto output =
        pipeline.linear_network({{poly.output, 0}, {input, 0}}, {1, 0}, 1, [&](double frequency) {
            ++calls[frequency];
            return transfer(Complex{.3, -.2});
        });
    const std::map<double, std::size_t> expected_calls{{4 * spacing, 1},
                                                       {8 * spacing, 1},
                                                       {12 * spacing, 1},
                                                       {16 * spacing, 1},
                                                       {20 * spacing, 1},
                                                       {24 * spacing, 1}};
    require(
        calls == expected_calls,
        "one-output network extraction must include each actual generated frequency exactly once");
    auto expected = polynomial_reference({{8, wide(a)}, {12, wide(b)}}, {0., 1., .5});
    expected[8] += wide(a);
    expected[12] += wide(b);
    for (auto &entry : expected) {
        entry.second *= Wide{.3L, -.2L};
    }
    check_spectrum(pipeline.inspect(output), expected, "network transfer of generated histories");
    require(before.records.size() == 6, "network mutated polynomial input snapshot");
    check_support(pipeline, pipeline.inspect(output));

    const auto empty = pipeline.seed({});
    calls.clear();
    const auto empty_result =
        pipeline.linear_network({{empty, 0}}, {0, 1}, 1, [&](double frequency) {
            ++calls[frequency];
            return transfer(1.);
        });
    require(pipeline.inspect(empty_result).records.empty() &&
                calls == std::map<double, std::size_t>{{0., 1}},
            "empty network must still build and validate reference at zero frequency");
    rejects<std::invalid_argument>([&] {
        pipeline.linear_network({{empty, 9}}, {0, 1}, 1, [](double) {
            return transfer(1.);
        });
    });
    rejects<std::exception>([&] {
        pipeline.linear_network({{empty, 0}}, {0, 9}, 9, [](double) {
            return transfer(1.);
        });
    });
    rejects<std::invalid_argument>([&] {
        pipeline.linear_network({{empty, 0}}, {0, 1}, 1, [](double) {
            return transfer(1., 75.);
        });
    });
    rejects<std::invalid_argument>([&] {
        pipeline.linear_network({{empty, 0}}, {0, 1}, 1, {});
    });
}
} // namespace

int main() {
    struct Case {
        const char *name;
        void (*run)();
    };

    const Case cases[]{
        {"mixed polynomial convolution", mixed_polynomial_matches_convolution},
        {"cancelled history recursion", cancelled_histories_survive_recursion},
        {"stream-local complex combiner", stream_locality_and_complex_combiner},
        {"repeated full-history conjugation", repeated_mixer_conjugates_all_history},
        {"polynomial mixer polynomial chain", polynomial_mixer_polynomial_chain},
        {"hand-counted source-order cutoffs", hand_counted_source_order_cutoffs},
        {"shared physical compression drive", shared_physical_drive_and_compression},
        {"filtered zero-drive cancellation", filtered_cancellation_zero_drive},
        {"independent RF and LO identities", independent_sources_and_lo_groups},
        {"fractional support and compensated sum", fractional_support_and_compensated_sum},
        {"actual-frequency and empty network validation",
         generated_frequencies_and_empty_network_validation}};
    for (const auto &test : cases) {
        try {
            test.run();
        } catch (const std::exception &error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
}
