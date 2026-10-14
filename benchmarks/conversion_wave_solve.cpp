// Developer benchmark, deliberately outside CTest: no timing pass/fail threshold.
// Compile twice, once against the baseline and once against the changed headers.
// Define RFMODEL_BENCH_BASELINE for the baseline (full analyze(false) candidates).
#include "rfmodel/conversion/conversion_operating_point.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>

using namespace rfmodel;
using Clock = std::chrono::steady_clock;

static double checksum(const std::vector<Complex> &wave) {
    double value = 0.;
    for (std::size_t i = 0; i < wave.size(); ++i) {
        value += (i + 1.) * (wave[i].real() + .31 * wave[i].imag());
    }
    return value;
}

static void extreme_benchmark() {
    // Separate fallback range: Q*s underflows although the legacy result is representable.
    FrequencyConversionModel model(1., {{0, 1}}, {1, {1e-200}}, {1, {0.}});
    const auto noise = model.zero_noise();
    std::vector<double> samples;
    for (int repeat = 0; repeat < 5; ++repeat) {
        const auto begin = Clock::now();
#ifdef RFMODEL_BENCH_BASELINE
        const auto result = model.analyze({1e-130}, {(1. - 1e-12) / 1e-200}, noise, noise);
#else
        const auto result = model.solve_waves({1e-130}, {(1. - 1e-12) / 1e-200});
#endif
        const auto elapsed =
            std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        samples.push_back(elapsed);
        std::cout << "extreme_sample,1," << repeat << ',' << elapsed << ','
                  << result.incident[0].real() << ',' << result.outgoing[0].real() << ','
                  << result.relative_residual << '\n';
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "extreme_median,1," << samples[2] << '\n';
}

int main() {
    std::cout << std::setprecision(17);
    extreme_benchmark();
    for (const std::size_t n : {16, 64, 128, 256}) {
        std::vector<ConversionChannel> channels;
        std::vector<Complex> source(n), reflection(n), offset(n);
        auto direct = conversion_detail::zero(n), conjugate = direct;
        for (std::size_t i = 0; i < n; ++i) {
            channels.push_back({i, 1});
            source[i] = {.2 + .01 * (i % 7), -.1 + .005 * (i % 5)};
            reflection[i] = {.3, .04};
            offset[i] = {.01, -.02};
            for (std::size_t j = 0; j < n; ++j) {
                direct(i, j) = i == j ? Complex{.45, .03} : Complex{.04 / n, -.02 / n};
                conjugate(i, j) = i == j ? Complex{.03, -.01} : Complex{-.01 / n, .005 / n};
            }
        }
        const FrequencyConversionModel model(1., channels, direct, conjugate);
        auto noise = model.zero_noise();
        for (std::size_t i = 0; i < n; ++i) {
            noise.covariance(i, i) = 1e-20;
            noise.complementary(i, i) = 2e-21;
        }
        ConversionDevice device{model, source, reflection, noise, noise};
        FrequencyConversionNetwork network(1.);
        network.add(device);
        std::vector<double> linear_times, nonlinear_times;
        for (int repeat = 0; repeat < 5; ++repeat) {
            auto begin = Clock::now();
#ifdef RFMODEL_BENCH_BASELINE
            const auto linear = network.analyze(false, nullptr, offset);
#else
            const auto linear = network.solve_waves(offset);
#endif
            const auto linear_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            linear_times.push_back(linear_ms);
            std::vector<double> trace;
            const ConversionNonlinearDevice nonlinear{
                0, [&](const std::vector<Complex> &incident) {
                    trace.push_back(checksum(incident));
                    auto a = direct, b = conjugate;
                    std::vector<Complex> outgoing(n);
                    for (std::size_t i = 0; i < n; ++i) {
                        for (std::size_t j = 0; j < n; ++j) {
                            outgoing[i] += direct(i, j) * incident[j] +
                                           conjugate(i, j) * std::conj(incident[j]);
                        }
                        // Cubic saturation, with exact Wirtinger derivatives.
                        outgoing[i] -= .8 * std::norm(incident[i]) * incident[i];
                        a(i, i) -= 1.6 * std::norm(incident[i]);
                        b(i, i) -= .8 * incident[i] * incident[i];
                    }
                    return ConversionLinearization{
                        FrequencyConversionModel(1., channels, std::move(a), std::move(b)),
                        std::move(outgoing)};
                }};
            begin = Clock::now();
            const auto result = solve_conversion_operating_point(
                {device}, {}, {nonlinear}, std::vector<Complex>(n), {}, false, nullptr, offset);
            const auto nonlinear_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            nonlinear_times.push_back(nonlinear_ms);
            std::cout << "sample," << n << ',' << repeat << ',' << linear_ms << ',' << nonlinear_ms
                      << ',' << linear.relative_residual << ',' << result.waves.relative_residual
                      << ',' << result.scaled_residual << ',' << result.iterations << ','
                      << result.backtracks << ',' << checksum(linear.incident) << ','
                      << checksum(result.waves.incident);
            for (auto value : trace) {
                std::cout << ',' << value;
            }
            std::cout << '\n' << std::flush;
        }
        std::sort(linear_times.begin(), linear_times.end());
        std::sort(nonlinear_times.begin(), nonlinear_times.end());
        std::cout << "median," << n << ',' << linear_times[2] << ',' << nonlinear_times[2] << '\n'
                  << std::flush;
    }
}
