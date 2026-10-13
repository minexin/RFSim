// C++17, header-only. Link with RFModel::rfmodel; no C ABI, Python, or JSON.
#include <rfmodel/coherent_pipeline.hpp>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
const rfmodel::StreamRecord &at_bin(const rfmodel::StreamSnapshot &snapshot, int bin) {
    for (const auto &record : snapshot.records) {
        if (record.component.bin == bin) {
            return record;
        }
    }
    throw std::runtime_error("expected example bin is missing");
}
} // namespace

int main() {
    try {
        using namespace rfmodel;
        constexpr double spacing_hz = 1e8;
        // Reserve RF group 1 and LO group 2 before the first operation.
        CoherentPipeline pipeline(spacing_hz, 50., 2);
        const auto source = pipeline.seed({{8, SpectrumKind::source, 1., 1, {.01, 0.}},
                                           {12, SpectrumKind::source, 1., 1, {-.01, 0.}}});
        const auto mixed = pipeline.mix(source, {10, 2, 0., 0.});
        const auto &folded = at_bin(pipeline.inspect(mixed), 2);
        std::cout << std::setprecision(10) << "folded IF: wave=" << folded.component.amplitude
                  << ", histories=" << folded.history.size() << '\n';

        // One requested output. This ideal IF-only filter is constructed at
        // the actual incident frequencies (including bins with a zero wave).
        const auto filtered = pipeline.linear_network(
            {{mixed, 0}}, {0, 1}, 1, [spacing_hz = spacing_hz](double frequency_hz) {
                const double transfer = frequency_hz == 2 * spacing_hz ? 1. : 0.;
                LinearNetwork network(50.);
                network.add({2, {0., transfer, transfer, 0.}});
                return network;
            });

        // Branches reuse the same immutable filtered stream.
        const auto compressed = pipeline.compress_fundamentals(
            filtered, SaturatingFundamentalCompression(20., 20., 23.));
        const auto &compressed_if = at_bin(pipeline.inspect(compressed.output), 2);
        std::cout << "compression: physical drive=" << compressed.input_power_w
                  << " W, common gain=" << compressed.common_amplitude_gain << '\n';
        std::cout << "compressed IF histories:";
        for (const auto &term : compressed_if.history) {
            std::cout << ' ' << term.amplitude;
        }
        std::cout << '\n';

        // y = v + 0.5*v^2 at 50 ohms; source order includes RF and LO factors.
        const auto polynomial = pipeline.polynomial(filtered, {0., 1., .5}, 4);
        const auto &second_harmonic = at_bin(pipeline.inspect(polynomial.output), 4);
        std::cout << "quadratic IF harmonic: wave=" << second_harmonic.component.amplitude
                  << ", histories=" << second_harmonic.history.size() << '\n';
        std::cout << "polynomial: physical inputs=" << polynomial.counts.physical_input_count
                  << ", expanded inputs=" << polynomial.counts.expanded_input_count
                  << ", generated paths=" << polynomial.counts.generated_term_count
                  << ", discarded paths=" << polynomial.counts.discarded_term_count << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "coherent pipeline example: " << error.what() << '\n';
        return 1;
    }
}
