// Installed public headers only. This file and coherent_pipeline_probe.cpp
// deliberately instantiate the same inline API in separate translation units.
#include <rfmodel/coherent/coherent_pipeline.hpp>
#include <cmath>
#include <complex>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

bool coherent_pipeline_odr_probe();

namespace {
namespace rf = rfmodel;
using Spectrum = std::map<int, std::complex<long double>>;
std::size_t checks = 0;

void require(bool condition, const std::string &what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(what);
    }
}

void close(rf::Complex actual, rf::Complex expected, const std::string &what) {
    const double error = std::abs(actual - expected);
    require(std::isfinite(error) && error <= 1e-16 + 1e-11 * std::abs(expected), what);
}

const rf::StreamRecord &source_bin(const rf::StreamSnapshot &snapshot, int bin) {
    for (const auto &record : snapshot.records) {
        if (record.component.bin == bin && record.component.kind == rf::SpectrumKind::source) {
            return record;
        }
    }
    throw std::runtime_error("source bin missing: " + std::to_string(bin));
}

// Independent ordered signed-spectrum convolution, with RF projection.
// V[k] = sqrt(R/2)*a[k]; quadratic voltage products include both orders.
// This oracle does not call the pipeline, polynomial, reducer, or composer.
Spectrum square_law(const std::map<int, rf::Complex> &input) {
    Spectrum voltage;
    for (const auto &entry : input) {
        const std::complex<long double> value{entry.second.real(), entry.second.imag()};
        voltage[entry.first] = 5.L * value;
        voltage[-entry.first] = std::conj(voltage[entry.first]);
    }
    Spectrum expected;
    for (const auto &entry : voltage) {
        if (entry.first > 0) {
            expected[entry.first] += entry.second / 5.L;
        }
        for (const auto &other : voltage) {
            const int bin = entry.first + other.first;
            if (bin > 0) {
                expected[bin] += .5L * entry.second * other.second / 5.L;
            }
        }
    }
    return expected;
}

void check_mixed_chain() {
    constexpr double spacing = 1e8;
    const rf::Complex a{.01, .003}, b{.02, -.002};
    const auto pump = std::polar(1., .3);
    const std::map<int, rf::Complex> expected_mixed{
        {2, std::conj(a) * pump + b * std::conj(pump)}, {18, a * pump}, {22, b * pump}};
    rf::CoherentPipeline pipeline(spacing, 50., 2);
    const auto seed = pipeline.seed(
        {{8, rf::SpectrumKind::source, 1., 1, a}, {12, rf::SpectrumKind::source, 1., 1, b}});
    const auto mixed = pipeline.mix(seed, {10, 2, 0., .3});
    const auto &snapshot = pipeline.inspect(mixed);
    require(snapshot.records.size() == 3, "two mixer paths fold to one IF component");
    require(source_bin(snapshot, 2).history.size() == 2, "folded IF retains both histories");
    for (const auto &entry : expected_mixed) {
        close(source_bin(snapshot, entry.first).component.amplitude,
              entry.second,
              "direct mixer wave at bin " + std::to_string(entry.first));
    }
    const double physical_drive = std::norm(expected_mixed.at(2)) + std::norm(a) + std::norm(b);
    close(snapshot.reduction.total_power_w, physical_drive, "restored physical mixer power");
    for (const auto &record : snapshot.records) {
        require(record.physical_identity.factors.size() == 1, "rebased mixed physical root");
        const auto physical = pipeline.physical_root(record.physical_identity.factors[0].root_id);
        require(physical.role == rf::RootRole::mixed && physical.bin == record.component.bin,
                "mixed physical root metadata");
        for (const auto &term : record.history) {
            require(term.factors.size() == 2, "mixer source order is two");
            std::size_t lo_count = 0;
            for (const auto &factor : term.factors) {
                const auto root = pipeline.history_root(factor.root_id);
                if (root.role == rf::RootRole::lo) {
                    ++lo_count;
                    require(root.bin == 10 && root.bandwidth_hz == 0., "LO history metadata");
                }
            }
            require(lo_count == 1, "exactly one LO factor per first mixing history");
        }
    }

    const auto polynomial = pipeline.polynomial(mixed, {0., 1., .5}, 4);
    require(polynomial.counts.physical_input_count == 3 &&
                polynomial.counts.expanded_input_count == 4 &&
                polynomial.counts.generated_term_count == 19 &&
                polynomial.counts.discarded_term_count == 0,
            "hand-counted mixed polynomial generation");
    std::map<int, rf::Complex> actual;
    for (const auto &record : pipeline.inspect(polynomial.output).records) {
        actual[record.component.bin] += record.component.amplitude;
    }
    const auto expected = square_law(expected_mixed);
    require(actual.size() == expected.size(), "independent polynomial RF support");
    for (const auto &entry : expected) {
        close(actual.at(entry.first),
              {static_cast<double>(entry.second.real()), static_cast<double>(entry.second.imag())},
              "independent convolution bin " + std::to_string(entry.first));
    }
    const auto cutoff = pipeline.polynomial(mixed, {0., 1., .5}, 3);
    require(cutoff.counts.generated_term_count == 19 && cutoff.counts.discarded_term_count == 15 &&
                cutoff.counts.discarded_by_source_order.at(4) == 15,
            "global source order is independent of local polynomial degree");

    // This also exercises network callbacks at generated, not just source, bins.
    const rf::Complex transfer{.5, .25};
    std::set<int> visited;
    const auto filtered =
        pipeline.linear_network({{polynomial.output, 0}}, {0, 1}, 1, [&](double frequency) {
            const int bin = static_cast<int>(std::llround(frequency / spacing));
            require(visited.insert(bin).second, "network builder called once per distinct bin");
            rf::LinearNetwork network(50.);
            network.add({2, {0., transfer, transfer, 0.}});
            return network;
        });
    std::set<int> expected_bins;
    std::map<int, rf::Complex> transmitted;
    for (const auto &entry : expected) {
        expected_bins.insert(entry.first);
    }
    for (const auto &record : pipeline.inspect(filtered).records) {
        transmitted[record.component.bin] += record.component.amplitude;
    }
    require(visited == expected_bins, "builder visits actual generated RF bins");
    for (const auto &entry : actual) {
        close(transmitted.at(entry.first),
              transfer * entry.second,
              "linear transfer after expansion");
    }
    require(&pipeline.inspect(mixed) == &snapshot && snapshot.records.size() == 3,
            "earlier snapshot reference stays valid");
    close(snapshot.reduction.total_power_w, physical_drive, "later branches do not mutate input");
}

const rf::StreamRecord &
component(const rf::StreamSnapshot &snapshot, int bin, rf::SpectrumKind kind) {
    for (const auto &record : snapshot.records) {
        if (record.component.bin == bin && record.component.kind == kind) {
            return record;
        }
    }
    throw std::runtime_error("amplifier component missing");
}

// This fixture stays below both anchors. Its expected gain and voltage products
// are scalar formulae, independent of the amplifier/polynomial/reduction APIs.
void check_amplifier_adapters() {
    const rf::Complex wave{.001, -.002};
    const double power = std::norm(wave);
    const auto gain = [](double drive) {
        const long double p1 = std::pow(10.L, -.9L); // OP1dB=20, G=0.
        return static_cast<double>(1.L - (1.L - std::pow(10.L, -.05L)) * drive / p1);
    };
    rf::CoherentPipeline pipeline(1e8, 50., 1);
    const auto seed = pipeline.seed({{2, rf::SpectrumKind::source, 1., 1, wave}});
    const rf::CoherentLimitedAmplifier limited_model(0., 20., 23., 30., 20., 50.);
    const auto limited = pipeline.limited_amplifier(seed, limited_model);
    const auto &first = pipeline.inspect(limited.output);
    const rf::Complex source = gain(power) * wave;
    // IIP2=1 W and IIP3=.1 W: a2=1/sqrt(2*50)=.1,
    // a3=-2/(3*50*.1); sqrt(R/2)=5, and cubic carrier multiplicity=3.
    const rf::Complex h2 = .5 * wave * wave;
    const rf::Complex h3 = (-10. / 3.) * wave * wave * wave;
    const rf::Complex im3 = -10. * wave * wave * std::conj(wave);
    require(first.records.size() == 4, "limited source, H2, H3 and carrier IM3 identities");
    close(limited.input_power_w, power, "limited physical common drive");
    close(limited.operating_point.fundamental_amplitude_gain,
          gain(power),
          "limited independently computed common gain");
    close(limited.operating_point.nonlinear_input_scale, 1., "limited below-knee scale");
    close(limited.operating_point.limited_input_power_w, power, "limited below-knee power");
    close(component(first, 2, rf::SpectrumKind::source).component.amplitude,
          source,
          "limited direct source");
    close(component(first, 4, rf::SpectrumKind::harmonic).component.amplitude,
          h2,
          "limited independent H2 voltage product");
    close(component(first, 6, rf::SpectrumKind::harmonic).component.amplitude,
          h3,
          "limited independent H3 voltage product");
    close(component(first, 2, rf::SpectrumKind::intermod).component.amplitude,
          im3,
          "limited keeps nonlinear carrier identity separate");
    require(limited.direct_contribution_count == 1 && limited.discarded_direct_contributions == 0 &&
                limited.nonlinear_counts.physical_input_count == 1 &&
                limited.nonlinear_counts.expanded_input_count == 1 &&
                limited.nonlinear_counts.generated_term_count == 3 &&
                limited.nonlinear_counts.discarded_term_count == 0,
            "limited direct and nonlinear path counts");
    const double first_power = std::norm(source) + std::norm(h2) + std::norm(h3) + std::norm(im3);
    const auto cascade = pipeline.limited_amplifier(
        limited.output, limited_model, rf::AmplifierInputMode::propagate_distortion);
    const auto &second = pipeline.inspect(cascade.output);
    close(cascade.input_power_w, first_power, "limited cascade includes distortion in drive");
    require(second.records.size() == 4 && cascade.direct_contribution_count == 4 &&
                cascade.nonlinear_counts.physical_input_count == 1 &&
                cascade.nonlinear_counts.generated_term_count == 3,
            "limited cascade conducts distortion without secondary generation");
    for (const auto &record : first.records) {
        require(component(second, record.component.bin, record.component.kind)
                        .component.coherence_group == record.component.coherence_group,
                "limited cascade reuses physical identity");
    }
    close(component(second, 4, rf::SpectrumKind::harmonic).component.amplitude,
          gain(first_power) * h2 + .5 * source * source,
          "limited conducted and newly generated H2 interfere");

    const rf::CoherentHighOrderAmplifier highorder_model(0., 20., 23., {.5}, 50.);
    const auto highorder = pipeline.highorder_amplifier(seed, highorder_model, 2);
    const auto &high_first = pipeline.inspect(highorder.output);
    const rf::Complex high_h2 = 2.5 * wave * wave;
    require(high_first.records.size() == 2 && highorder.direct_contribution_count == 1 &&
                highorder.nonlinear_counts.generated_term_count == 1,
            "high-order explicit quadratic source-only path");
    close(component(high_first, 4, rf::SpectrumKind::harmonic).component.amplitude,
          high_h2,
          "high-order independent voltage normalization");
    const double high_power = std::norm(source) + std::norm(high_h2);
    const auto high_cascade = pipeline.highorder_amplifier(
        highorder.output, highorder_model, 2, rf::AmplifierInputMode::propagate_distortion);
    const auto &high_second = pipeline.inspect(high_cascade.output);
    require(high_second.records.size() == 2 &&
                high_cascade.nonlinear_counts.physical_input_count == 1 &&
                high_cascade.nonlinear_counts.generated_term_count == 1,
            "high-order cascade excludes distortion from generation");
    close(high_cascade.input_power_w, high_power, "high-order full physical cascade drive");
    close(component(high_second, 4, rf::SpectrumKind::harmonic).component.amplitude,
          gain(high_power) * high_h2 + 2.5 * source * source,
          "high-order conducted/new H2 reconciliation");
    require(component(high_second, 4, rf::SpectrumKind::harmonic).component.coherence_group ==
                component(high_first, 4, rf::SpectrumKind::harmonic).component.coherence_group,
            "high-order cascade reuses harmonic identity");

    const auto cut = pipeline.highorder_amplifier(
        highorder.output, highorder_model, 1, rf::AmplifierInputMode::propagate_distortion);
    require(pipeline.inspect(cut.output).records.size() == 1 &&
                cut.direct_contribution_count == 2 && cut.discarded_direct_contributions == 1 &&
                cut.nonlinear_counts.generated_term_count == 1 &&
                cut.nonlinear_counts.discarded_term_count == 1 &&
                cut.nonlinear_counts.discarded_by_source_order.at(2) == 1,
            "high-order cutoff counts direct and generated histories independently");
    close(cut.input_power_w, high_power, "cutoff cannot lower operating-point input drive");
    close(cut.operating_point.fundamental_amplitude_gain,
          gain(high_power),
          "cutoff gain uses uncut physical input");
    close(source_bin(pipeline.inspect(cut.output), 2).component.amplitude,
          gain(high_power) * source,
          "cutoff retains source with full-drive gain");
    require(&pipeline.inspect(limited.output) == &first && first.records.size() == 4,
            "later amplifier calls preserve prior snapshots");
}
} // namespace

int main() {
    try {
        require(coherent_pipeline_odr_probe(), "second translation unit cancellation probe");
        check_mixed_chain();
        check_amplifier_adapters();
        std::cout << "PASS: " << checks << " installed coherent pipeline checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "coherent pipeline consumer: " << error.what() << '\n';
        return 1;
    }
}
