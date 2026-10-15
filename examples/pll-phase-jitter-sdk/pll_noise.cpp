#include "pll_noise.hpp"
#include <rfmodel/conversion/conversion_phase_response.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace example {
namespace {
using namespace rfmodel;
constexpr int carrier_bin = 100000000;
constexpr double reference_psd = 2e-14; // One-sided rad^2/Hz, common reference.
constexpr double vco_psd = 5e-13;       // Each VCO has this PSD, independently.
const std::array<Complex, 2> carrier_waves{{{0.1, 0.}, {0.06, 0.08}}}; // sqrt(W)

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(Complex value, Complex expected, double scale) {
    if (!(std::abs(value - expected) <= 2e-11 * std::max(scale, std::abs(expected)))) {
        std::ostringstream detail;
        detail << "independent numerical oracle failed: actual=" << value
               << " expected=" << expected << " scale=" << scale;
        throw std::runtime_error(detail.str());
    }
}

// Independent Fourier coefficient oracle: three proper, independent latent
// sources. Upper = j*a*H*sqrt(S/2); lower = j*a*conj(H)*sqrt(S/2).
ConversionNoise oracle(const std::array<LockedPiPllResult, 2> &pll) {
    ConversionNoise result{SMatrix{6, std::vector<Complex>(36)},
                           SMatrix{6, std::vector<Complex>(36)}};
    for (int process = 0; process < 3; ++process) {
        std::array<Complex, 6> u{}, v{};
        for (std::size_t k = 0; k < 2; ++k) {
            if (process != 0 && process != static_cast<int>(k) + 1) {
                continue;
            }
            const auto h = process == 0 ? pll[k].transfer.reference : pll[k].transfer.vco;
            const auto amplitude = Complex{0., 1.} * carrier_waves[k] *
                                   std::sqrt((process == 0 ? reference_psd : vco_psd) / 2.);
            u[3 * k + 2] = amplitude * h;
            v[3 * k] = amplitude * std::conj(h);
        }
        for (std::size_t i = 0; i < 6; ++i) {
            for (std::size_t j = 0; j < 6; ++j) {
                result.covariance(i, j) += u[i] * std::conj(u[j]) + v[i] * std::conj(v[j]);
                result.complementary(i, j) += u[i] * v[j] + v[i] * u[j];
            }
        }
    }
    return result;
}
} // namespace

Result simulate(const std::vector<int> &offsets_hz, bool equal_loops) {
    require(offsets_hz.size() >= 2, "at least two offsets required");
    int previous = 0;
    for (int offset : offsets_hz) {
        require(offset > previous && offset < carrier_bin, "invalid positive offset grid");
        previous = offset;
    }
    // Both outputs are explicitly declared 100 MHz, with N=10 (10 MHz reference).
    // Changing R/C changes loop dynamics, not the declared output carrier.
    const LockedPiPllParameters a{1e-3, 2. * std::acos(-1.) * 1e7, 10., 100., 1e-8};
    auto b = a;
    if (!equal_loops) {
        b.resistance_ohms = 150.;
        b.capacitance_f = 2e-8;
    }
    Result result;
    result.carrier_hz = carrier_bin;
    std::array<std::vector<PhaseNoiseDensitySample>, 2> phase_samples;
    std::array<double, 2> independent_integral{};
    for (int offset : offsets_hz) {
        OffsetResult row;
        row.offset_hz = offset;
        row.pll = {{analyze_locked_pi_pll(a, offset, {reference_psd, 0., vco_psd, 0., 0.}),
                    analyze_locked_pi_pll(b, offset, {reference_psd, 0., vco_psd, 0., 0.})}};
        const std::vector<ConversionChannel> channels{{0, carrier_bin - offset},
                                                      {0, carrier_bin},
                                                      {0, carrier_bin + offset},
                                                      {1, carrier_bin - offset},
                                                      {1, carrier_bin},
                                                      {1, carrier_bin + offset}};
        const SMatrix zero{6, std::vector<Complex>(36)};
        // Two matched, uncoupled loads. Incident waves are the PLL outputs;
        // outgoing waves are zero. No hidden physical-clock inference.
        const FrequencyConversionModel model(1., channels, zero, zero);
        std::vector<Complex> source(6);
        source[1] = carrier_waves[0];
        source[4] = carrier_waves[1];
        ConversionSimulationRequest request;
        request.loaded_noise = true;
        request.devices.push_back(
            {model, source, std::vector<Complex>(6), model.zero_noise(), model.zero_noise()});
        const auto ready = with_phase_response_noise(
            request,
            {{"explicit common reference",
              {{{0, 0, carrier_bin}}, {{0, 1, carrier_bin}}},
              {{offset,
                double(offset),
                reference_psd,
                {row.pll[0].transfer.reference, row.pll[1].transfer.reference}}}},
             {"independent VCO A",
              {{{0, 0, carrier_bin}}},
              {{offset, double(offset), vco_psd, {row.pll[0].transfer.vco}}}},
             {"independent VCO B",
              {{{0, 1, carrier_bin}}},
              {{offset, double(offset), vco_psd, {row.pll[1].transfer.vco}}}}});
        const auto solved = analyze_conversion_simulation(ready);
        row.incident_noise = solved.waves.incident_noise;
        require(row.incident_noise.covariance.values.size() == 36 &&
                    row.incident_noise.complementary.values.size() == 36,
                "loaded incident C/P must contain six channels");
        const auto expected = oracle(row.pll);
        double matrix_scale = 0.;
        for (const auto &value : expected.covariance.values) {
            matrix_scale = std::max(matrix_scale, std::abs(value));
        }
        for (std::size_t i = 0; i < 36; ++i) {
            near(row.incident_noise.covariance.values[i],
                 expected.covariance.values[i],
                 matrix_scale);
            near(row.incident_noise.complementary.values[i],
                 expected.complementary.values[i],
                 matrix_scale);
        }
        const auto ha = row.pll[0].transfer.reference, hb = row.pll[1].transfer.reference;
        row.cross_phase_rad2_per_hz = ha * std::conj(hb) * reference_psd;
        row.differential_common_rad2_per_hz = std::norm(ha - hb) * reference_psd;
        row.differential_vco_rad2_per_hz =
            row.pll[0].noise.vco_rad2_per_hz + row.pll[1].noise.vco_rad2_per_hz;
        require(row.differential_vco_rad2_per_hz > 0., "independent VCO noise must remain");
        near(row.pll[0].noise.reference_rad2_per_hz + row.pll[1].noise.reference_rad2_per_hz -
                 2. * row.cross_phase_rad2_per_hz.real(),
             row.differential_common_rad2_per_hz,
             1e-10);
        if (equal_loops) {
            require(row.differential_common_rad2_per_hz == 0., "equal shared H must cancel");
        }
        // This normalization is a check ONLY for this explicitly pure-PM,
        // matched-source example. General W/Hz is not a phase PSD.
        near(row.incident_noise.covariance(2, 5),
             carrier_waves[0] * std::conj(carrier_waves[1]) * row.cross_phase_rad2_per_hz / 2.,
             1e-28);
        for (std::size_t k = 0; k < 2; ++k) {
            const double phase_psd = row.pll[k].noise.total_rad2_per_hz;
            near(row.incident_noise.covariance(3 * k + 2, 3 * k + 2),
                 std::norm(carrier_waves[k]) * phase_psd / 2.,
                 1e-28);
            // Jitter uses the explicit rad^2/Hz budget, never inferred W/Hz.
            phase_samples[k].push_back({double(offset), phase_psd});
            if (!result.offsets.empty()) {
                const auto &last = result.offsets.back();
                const double old_psd = std::norm(last.pll[k].transfer.reference) * reference_psd +
                                       std::norm(last.pll[k].transfer.vco) * vco_psd;
                const double new_psd = std::norm(row.pll[k].transfer.reference) * reference_psd +
                                       std::norm(row.pll[k].transfer.vco) * vco_psd;
                independent_integral[k] += (offset - last.offset_hz) * (old_psd + new_psd) / 2.;
            }
        }
        result.offsets.push_back(std::move(row));
    }
    for (std::size_t k = 0; k < 2; ++k) {
        result.jitter[k] = measure_phase_jitter({phase_samples[k],
                                                 result.carrier_hz,
                                                 double(offsets_hz.front()),
                                                 double(offsets_hz.back()),
                                                 PhaseJitterIntegrationPolicy::linear_rad2_per_hz});
        near(result.jitter[k].phase_variance_rad2, independent_integral[k], 1e-28);
        near(result.jitter[k].rms_phase_rad, std::sqrt(independent_integral[k]), 1e-28);
        near(result.jitter[k].rms_time_s,
             std::sqrt(independent_integral[k]) / (2. * std::acos(-1.) * result.carrier_hz),
             1e-28);
    }
    return result;
}
} // namespace example
