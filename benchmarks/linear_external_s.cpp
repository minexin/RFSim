// Developer benchmark, outside CTest; no machine-dependent timing threshold.
// Both paths use the identical network and selected RHS count, never an inverse.
#include "rfmodel/linear/network.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>

using namespace rfmodel;
using Clock = std::chrono::steady_clock;

static SMatrix repeated_solve(const LinearNetwork &network, const std::vector<std::size_t> &ports) {
    SMatrix result{ports.size(), std::vector<Complex>(ports.size() * ports.size())};
    for (std::size_t c = 0; c < ports.size(); ++c) {
        auto copy = network;
        for (std::size_t i = 0; i < ports.size(); ++i) {
            copy.terminate(ports[i], {}, i == c ? Complex{1., 0.} : Complex{});
        }
        const auto waves = copy.solve();
        for (std::size_t r = 0; r < ports.size(); ++r) {
            result(r, c) = waves.outgoing[ports[r]];
        }
    }
    return result;
}

static double checksum(const SMatrix &s) {
    double result = 0.;
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        result += (1. + i) * (s.values[i].real() + .31 * s.values[i].imag());
    }
    return result;
}

static void run(std::size_t n, std::size_t m) {
    SMatrix s{n, std::vector<Complex>(n * n)};
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            s(r, c) = Complex{std::sin(1. + 17. * r + 3. * c), std::cos(3. + 7. * r + 11. * c)} /
                      (2. * n);
        }
    }
    LinearNetwork network;
    std::vector<Complex> references(n);
    for (std::size_t p = 0; p < n; ++p) {
        references[p] = Complex{30. + p % 71, static_cast<double>(p % 31) - 15.};
    }
    network.add_power_waves(s, references);
    for (std::size_t p = m; p + 1 < n; p += 2) {
        network.connect(p, p + 1);
    }
    std::vector<std::size_t> ports(m);
    std::iota(ports.begin(), ports.end(), 0);
    const auto reference = repeated_solve(network, ports);
    const auto candidate = network.external_s(ports);
    double max_error = 0.;
    for (std::size_t i = 0; i < reference.values.size(); ++i) {
        max_error = std::max(max_error, std::abs(reference.values[i] - candidate.values[i]));
    }
    if (!std::isfinite(max_error) || max_error > 1e-12) {
        throw std::runtime_error("benchmark physics mismatch");
    }
    std::cout << "agreement," << n << ',' << m << ',' << max_error << ',' << checksum(reference)
              << ',' << checksum(candidate) << '\n';
    std::vector<double> legacy, factored;
    for (int repeat = 0; repeat < 7; ++repeat) {
        // Alternate measurement order after the warmup/agreement pass.
        for (int step = 0; step < 2; ++step) {
            const bool old = (repeat + step) % 2 == 0;
            const auto begin = Clock::now();
            const auto result = old ? repeated_solve(network, ports) : network.external_s(ports);
            const double ms =
                std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            (old ? legacy : factored).push_back(ms);
            std::cout << "sample," << n << ',' << m << ',' << (old ? "legacy" : "factored") << ','
                      << repeat << ',' << ms << ',' << checksum(result) << '\n';
        }
    }
    std::sort(legacy.begin(), legacy.end());
    std::sort(factored.begin(), factored.end());
    std::cout << "median," << n << ',' << m << ',' << legacy[3] << ',' << factored[3] << ','
              << legacy[3] / factored[3] << '\n';
}

int main() {
    std::cout << std::setprecision(17);
    run(96, 32);
    run(160, 2);
}
