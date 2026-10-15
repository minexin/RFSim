#include "rfmodel/noise/phase_jitter.hpp"
#include "../support/test_support.hpp"
#include <iostream>
#include <iomanip>
#include <type_traits>

using namespace rfmodel;

namespace {
constexpr auto policy = PhaseJitterIntegrationPolicy::linear_rad2_per_hz;

void relative(double actual, double expected, double tolerance = 4e-14) {
    if (!(std::isfinite(actual) && std::abs(actual / expected - 1.) <= tolerance)) {
        std::cerr << std::setprecision(17) << "actual=" << actual << " expected=" << expected
                  << " tolerance=" << tolerance << "\n";
    }
    require(std::isfinite(actual) && std::abs(actual / expected - 1.) <= tolerance,
            "phase jitter relative oracle mismatch");
}

PhaseJitterRequest flat(double density = 1e-12) {
    return {{{1e3, density}, {1e6, density}}, 1e8, 1e3, 1e6, policy};
}

void analytical() {
    static_assert(!std::is_default_constructible<PhaseJitterRequest>::value,
                  "carrier, band and policy must be explicit");
    const auto result = measure_phase_jitter(flat());
    relative(result.phase_variance_rad2, 9.99e-7);
    relative(result.rms_phase_rad, .0009994998749374609101);
    relative(result.rms_time_s, 1.5907534571602809633e-12);
    require(result.lower_offset_hz == 1e3 && result.upper_offset_hz == 1e6 &&
                result.carrier_hz == 1e8 && result.interpolation_intervals == 1 &&
                result.policy == policy,
            "result metadata");
    auto request = flat();
    request.carrier_hz *= 2.;
    const auto doubled = measure_phase_jitter(request);
    require(result.phase_variance_rad2 == doubled.phase_variance_rad2 &&
                result.rms_phase_rad == doubled.rms_phase_rad,
            "carrier must not change phase variance");
    relative(doubled.rms_time_s, result.rms_time_s / 2.);
    PhaseJitterRequest line{{{1., 5.}, {4., 14.}, {9., 29.}}, 1., 2., 7., policy};
    const auto clipped = measure_phase_jitter(line);
    relative(clipped.phase_variance_rad2, 77.5);
    require(clipped.interpolation_intervals == 2, "clipped interval count");
    auto left = line;
    left.upper_offset_hz = 4.;
    auto right = line;
    right.lower_offset_hz = 4.;
    relative(measure_phase_jitter(left).phase_variance_rad2 +
                 measure_phase_jitter(right).phase_variance_rad2,
             clipped.phase_variance_rad2);
    line.samples.insert(line.samples.begin() + 1, {2., 8.});
    relative(measure_phase_jitter(line).phase_variance_rad2, 77.5);
    for (auto &sample : line.samples) {
        sample.phase_psd_rad2_per_hz *= 1e-12;
    }
    const auto scaled = measure_phase_jitter(line);
    relative(scaled.phase_variance_rad2, 77.5e-12);
    relative(scaled.rms_phase_rad, clipped.rms_phase_rad * 1e-6);
    for (int power : {1, 2}) {
        PhaseJitterRequest sparse{
            {{1., 1.}, {10., power == 1 ? .1 : .01}, {100., power == 1 ? .01 : .0001}},
            100.,
            1.,
            100.,
            policy};
        relative(measure_phase_jitter(sparse).phase_variance_rad2, power == 1 ? 9.9 : 4.9995);
        const double continuous = power == 1 ? std::log(100.) : .99;
        double previous_error = 10.;
        for (int intervals : {20, 200, 2000}) {
            sparse.samples.clear();
            for (int i = 0; i <= intervals; ++i) {
                const double f = i == intervals ? 100. : std::pow(100., double(i) / intervals);
                sparse.samples.push_back({f, power == 1 ? 1. / f : 1. / (f * f)});
            }
            const double error = measure_phase_jitter(sparse).phase_variance_rad2 / continuous - 1.;
            require(error > 0. && error < previous_error / 50., "dense grid convergence");
            previous_error = error;
        }
        relative(previous_error, power == 1 ? 8.836499193283116e-7 : 2.65095022649e-6, 2e-9);
    }
    const double ssb = ssb_dbc_per_hz_to_phase_psd_rad2_per_hz(-120.);
    relative(ssb, 2e-12);
    // Independent 100-digit Decimal oracle for this exact binary64 dBc input.
    relative(ssb_dbc_per_hz_to_phase_psd_rad2_per_hz(-3079.5368556425274),
             2.2250738585073325888e-308);
    relative(measure_phase_jitter(flat(ssb)).phase_variance_rad2, 1.998e-6);
}

void validation() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double bad : {0., -1., nan, inf, -inf}) {
        for (int field = 0; field < 3; ++field) {
            auto q = flat(0.);
            double *fields[]{&q.carrier_hz, &q.lower_offset_hz, &q.upper_offset_hz};
            *fields[field] = bad;
            rejects<std::invalid_argument>([&] {
                measure_phase_jitter(q);
            });
        }
        auto q = flat(0.);
        q.samples.front().offset_hz = bad;
        rejects<std::invalid_argument>([&] {
            measure_phase_jitter(q);
        });
    }
    for (double bad : {-1., nan, inf, -inf}) {
        auto q = flat(0.);
        q.samples.push_back({2e6, bad}); // Out-of-band samples must still be validated.
        rejects<std::invalid_argument>([&] {
            measure_phase_jitter(q);
        });
    }
    for (double bad : {1e3, 500., nan, inf}) {
        auto q = flat(0.);
        q.samples.back().offset_hz = bad;
        rejects<std::invalid_argument>([&] {
            measure_phase_jitter(q);
        });
    }
    for (std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{1000001}}) {
        auto q = flat(0.);
        q.samples.resize(count);
        rejects<std::invalid_argument>([&] {
            measure_phase_jitter(q);
        });
    }
    auto q = flat(0.);
    q.upper_offset_hz = q.lower_offset_hz;
    rejects<std::invalid_argument>([&] {
        measure_phase_jitter(q);
    });
    q = flat(0.);
    q.policy = static_cast<PhaseJitterIntegrationPolicy>(99);
    rejects<std::invalid_argument>([&] {
        measure_phase_jitter(q);
    });
    q = flat(0.);
    q.lower_offset_hz = 1.;
    rejects<std::out_of_range>([&] {
        measure_phase_jitter(q);
    });
    q = flat(0.);
    q.upper_offset_hz = 2e6;
    rejects<std::out_of_range>([&] {
        measure_phase_jitter(q);
    });
    for (double bad : {nan, inf, -inf}) {
        rejects<std::invalid_argument>([&] {
            ssb_dbc_per_hz_to_phase_psd_rad2_per_hz(bad);
        });
    }
    for (double bad : {-1e300, 1e300}) {
        rejects<std::overflow_error>([&] {
            ssb_dbc_per_hz_to_phase_psd_rad2_per_hz(bad);
        });
    }
}

void binary_ranges() {
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double normal = std::numeric_limits<double>::min();
    const double huge = std::numeric_limits<double>::max();
    const double next = std::nextafter(1., 2.);
    // Adjacent clipping next to a zero endpoint must retain its triangular area.
    PhaseJitterRequest triangle{{{1., 0.}, {2., 2.}}, 1., 1., next, policy};
    relative(measure_phase_jitter(triangle).phase_variance_rad2, (next - 1.) * (next - 1.));
    triangle.samples = {{1., 2.}, {2., 0.}};
    triangle.lower_offset_hz = std::nextafter(2., 1.);
    triangle.upper_offset_hz = 2.;
    relative(measure_phase_jitter(triangle).phase_variance_rad2,
             (2. - triangle.lower_offset_hz) * (2. - triangle.lower_offset_hz));
    PhaseJitterRequest q{{{1., huge}, {2., huge}}, huge, 1., 2., policy};
    const auto large = measure_phase_jitter(q);
    relative(large.phase_variance_rad2, huge);
    relative(large.rms_time_s, 1.1870317946341340348e-155);
    // A subnormal density and enormous width produce a normal variance.
    q = {{{1., tiny}, {huge, tiny}}, 1., 1., huge, policy};
    relative(measure_phase_jitter(q).phase_variance_rad2, 8.881784197001251e-16);
    // Underflowing interpolation weight rescued by a huge endpoint density.
    q = {{{tiny, 0.}, {huge, huge}}, 1., 1., 2., policy};
    relative(measure_phase_jitter(q).phase_variance_rad2, 1.5);
    // An endpoint mean below denorm_min is retained before multiplying the width.
    q = {{{1., 0.}, {huge, tiny}}, 1., 1., huge, policy};
    relative(measure_phase_jitter(q).phase_variance_rad2, 4.4408920985006257e-16);
    q = {{{tiny, huge}, {2. * tiny, huge}}, 1., tiny, 2. * tiny, policy};
    relative(measure_phase_jitter(q).phase_variance_rad2, 8.881784197001251e-16);
    // Subnormal individual areas recover a normal total; no per-segment floor.
    q = {{{1., normal / 2.}, {2., normal / 2.}, {3., normal / 2.}}, 1., 1., 3., policy};
    require(measure_phase_jitter(q).phase_variance_rad2 == normal, "tiny area recovery");
    q = {{{1., huge}, {2., huge}, {3., huge}}, 1., 1., 3., policy};
    rejects<std::overflow_error>([&] {
        measure_phase_jitter(q);
    });
    q = {{{1., tiny}, {2., tiny}}, 1., 1., 2., policy};
    rejects<std::overflow_error>([&] {
        measure_phase_jitter(q);
    });
    q = {{{1., 1.}, {2., 1.}}, tiny, 1., 2., policy};
    rejects<std::overflow_error>([&] {
        measure_phase_jitter(q);
    });
    q = {{{1., normal}, {2., normal}}, huge, 1., 2., policy};
    rejects<std::overflow_error>([&] {
        measure_phase_jitter(q);
    });
    for (double carrier : {tiny, huge}) {
        q = {{{tiny, 0.}, {1., 0.}, {huge, 0.}}, carrier, tiny, huge, policy};
        const auto zero = measure_phase_jitter(q);
        require(zero.phase_variance_rad2 == 0. && zero.rms_phase_rad == 0. &&
                    zero.rms_time_s == 0. && zero.interpolation_intervals == 2,
                "zero PSD exact result with full validation");
    }
    q = {{{1., 1.}, {2., 0.}, {3., 1e-16}}, 1., 1., 10003., policy};
    for (int i = 4; i <= 10003; ++i) {
        q.samples.push_back({double(i), 1e-16});
    }
    relative(measure_phase_jitter(q).phase_variance_rad2, .50000000000100005, 5e-16);
    // The inclusive count budget accepts exactly one million valid samples.
    q.samples.clear();
    for (std::size_t i = 1; i <= 1000000; ++i) {
        q.samples.push_back({double(i), 0.});
    }
    q.lower_offset_hz = 1.;
    q.upper_offset_hz = 1000000.;
    require(measure_phase_jitter(q).interpolation_intervals == 999999, "inclusive count budget");
}

void ownership() {
    PhaseJitterMeasurement retained;
    {
        std::vector<PhaseNoiseDensitySample> source{{1., 1.}, {2., 1.}, {3., 1.}};
        PhaseJitterRequest q{source, 1., 1., 2., policy};
        source.clear();
        auto copy = q;
        q.samples.clear();
        retained = measure_phase_jitter(copy);
        auto snapshot = retained;
        copy.samples.back().phase_psd_rad2_per_hz = -1.;
        rejects<std::invalid_argument>([&] {
            retained = measure_phase_jitter(copy);
        });
        require(retained.phase_variance_rad2 == snapshot.phase_variance_rad2 &&
                    retained.rms_time_s == snapshot.rms_time_s,
                "failed assignment must preserve previous result");
        require(copy.samples.size() == 3 && copy.samples.back().phase_psd_rad2_per_hz == -1.,
                "measurement must not mutate input");
    }
    require(retained.phase_variance_rad2 == 1. && retained.interpolation_intervals == 1,
            "retained result must own its values");
    relative(retained.rms_time_s, .15915494309189533577);
}
} // namespace

int main() {
    try {
        analytical();
        validation();
        binary_ranges();
        ownership();
        std::cout << "phase jitter analytical, domain, range and ownership tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
