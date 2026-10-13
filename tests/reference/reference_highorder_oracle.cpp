// Test-only direct-kernel oracle. This deliberately has no request parser and no
// dependency on the replay probe. The JSON writer only transports kernel fields.
#include "rfmodel/coherent/coherent_amplifier.hpp"
#include "rfmodel/coherent/coherent_highorder_amplifier.hpp"
#include "rfmodel/coherent/coherent_polynomial.hpp"
#include "rfmodel/nonlinear/polynomial_intercepts.hpp"
#include "rfmodel/nonlinear/am_pm.hpp"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <thread>

namespace {
using namespace rfmodel;
bool first_case = true;

void component(const CoherentComponent &value) {
    std::cout << '[' << value.bin << ',' << static_cast<int>(value.kind) << ','
              << value.bandwidth_hz << ',' << value.coherence_group << ",["
              << value.amplitude.real() << ',' << value.amplitude.imag() << "]]";
}

void components(const std::vector<CoherentComponent> &values) {
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            std::cout << ',';
        }
        component(values[i]);
    }
    std::cout << ']';
}

void numbers(const std::vector<double> &values) {
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            std::cout << ',';
        }
        std::cout << values[i];
    }
    std::cout << ']';
}

void point(const AmplifierOperatingPoint &value) {
    std::cout << '[' << value.fundamental_amplitude_gain << ',' << value.nonlinear_input_scale
              << ',' << value.limited_input_power_w << ',' << value.quadratic_voltage_coefficient
              << ',' << value.cubic_voltage_coefficient << ']';
}

template <class Response> void response(const Response &value) {
    std::cout << "{\"inputs\":";
    components(value.inputs);
    std::cout << ",\"terms\":[";
    for (std::size_t i = 0; i < value.terms.size(); ++i) {
        if (i) {
            std::cout << ',';
        }
        const auto &term = value.terms[i];
        std::cout << '[' << term.order << ",[";
        for (int j = 0; j < term.order; ++j) {
            if (j) {
                std::cout << ',';
            }
            std::cout << term.input_indices[j];
        }
        std::cout << "],";
        component(term.component);
        std::cout << ']';
    }
    std::cout << ']';
}

void begin(const char *name, const char *operation) {
    if (!first_case) {
        std::cout << ',';
    }
    first_case = false;
    std::cout << "{\"name\":\"" << name << "\",\"operation\":\"" << operation << "\",";
}

void polynomial_case(const char *name,
                     double spacing,
                     double z,
                     std::uint64_t reserved,
                     const std::vector<double> &coefficients,
                     const std::vector<CoherentComponent> &inputs) {
    const auto actual = CoherentPolynomial(coefficients, z).evaluate(spacing, inputs, reserved);
    begin(name, "polynomial");
    std::cout << "\"spacing\":" << spacing << ",\"z\":" << z << ",\"reserved\":" << reserved
              << ",\"coefficients\":";
    numbers(coefficients);
    std::cout << ",\"inputs\":";
    components(inputs);
    std::cout << ",\"expected\":";
    response(actual);
    std::cout << "}}";
}

void highorder_case(const char *name,
                    double spacing,
                    double z,
                    std::uint64_t reserved,
                    const std::vector<double> &coefficients,
                    const std::vector<CoherentComponent> &inputs) {
    const auto actual = CoherentHighOrderAmplifier(10., 20., 23., coefficients, z)
                            .evaluate(spacing, inputs, reserved, false);
    begin(name, "highorder");
    std::cout << "\"spacing\":" << spacing << ",\"z\":" << z << ",\"reserved\":" << reserved
              << ",\"coefficients\":";
    numbers(coefficients);
    std::cout << ",\"inputs\":";
    components(inputs);
    std::cout << ",\"expected\":";
    response(actual);
    std::cout << ",\"total_input_power_w\":" << actual.total_input_power_w
              << ",\"operating_point\":";
    point(actual.operating_point);
    std::cout << "}}";
}
} // namespace

int main() {
    try {
        // Real bounded-pipe and process-failure coverage without platform-specific
        // shell scripts. Normal oracle execution never selects a fault mode.
        if (const char *fault = std::getenv("RFMODEL_REFERENCE_ORACLE_FAULT")) {
            const std::string mode(fault);
            if (mode == "timeout") {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                return 0;
            }
            if (mode == "stdout_limit" || mode == "stderr_limit") {
                auto *stream = mode == "stdout_limit" ? stdout : stderr;
                // Preserve the exact old payload sizes while avoiding thousands
                // of formatted insertions in this byte-limit fixture.
                std::size_t remaining = (mode == "stdout_limit" ? 16 * 1024 * 1024 : 65536) + 4096;
                const std::string block(65536, 'x');
                while (remaining != 0) {
                    const auto count = std::min(remaining, block.size());
                    if (std::fwrite(block.data(), 1, count, stream) != count) {
                        return 8;
                    }
                    remaining -= count;
                }
                return std::fflush(stream) == 0 ? 0 : 8;
            }
            if (mode == "nonzero") {
                return 7;
            }
            throw std::invalid_argument("unknown test fault mode");
        }
        std::cout.imbue(std::locale::classic());
        std::cout << std::setprecision(std::numeric_limits<double>::max_digits10) << '[';
        for (double peak : {-5., 0., 7.}) {
            for (double reference : {-20., 10., 40.}) {
                const auto power = std::pow(10., (reference - 30.) / 10.);
                for (double ratio : {0., .01, 1., 2.25, 4.}) {
                    begin("am-pm-branches", "am_pm");
                    const auto actual =
                        LegacyPointDerivativeAmPm(peak, reference).response(power * ratio);
                    std::cout << "\"power\":" << power * ratio << ",\"peak\":" << peak
                              << ",\"reference\":" << reference << ",\"expected\":";
                    numbers({actual.phase_rad, actual.radial_phase_slope});
                    std::cout << '}';
                }
            }
        }
        const std::vector<TwoToneIntercept> definitions{
            {1, 1, 30., 1, InterceptReference::input},
            {2, -1, 20., -1, InterceptReference::input},
            {6, -5, 33., 1, InterceptReference::output}};
        begin("mixed-intercepts-75-ohm", "intercepts");
        std::cout << "\"gain\":10,\"z\":75,\"definitions\":[[1,1,30,1,0],"
                     "[2,-1,20,-1,0],[6,-5,33,1,1]],\"expected\":";
        numbers(polynomial_coefficients_from_intercepts(10., definitions, 75.));
        std::cout << '}';
        begin("linear-intercepts", "intercepts");
        std::cout << "\"gain\":-3,\"z\":50,\"definitions\":[],\"expected\":";
        numbers(polynomial_coefficients_from_intercepts(-3., {}, 50.));
        std::cout << '}';
        const CoherentLimitedAmplifier limited(10., 20., 23., 30., 20., 75.);
        for (double drive : {0., .0001, .003, .1, 1e200}) {
            begin("operating-point", "operating_point");
            std::cout << "\"drive\":" << drive << ",\"z\":75,\"expected\":";
            point(limited.operating_point(drive));
            std::cout << '}';
        }
        const std::vector<CoherentComponent> phased{{13, SpectrumKind::source, 2., 9, {.05, -.03}},
                                                    {10, SpectrumKind::source, 1., 7, {.06, .02}}};
        const std::vector<CoherentComponent> merged{
            {10, SpectrumKind::source, 1., 7, {.01, .003}},
            {10, SpectrumKind::source, 1., 7, {.02, -.001}}};
        const std::vector<CoherentComponent> cancelled{
            {10, SpectrumKind::source, 1., 7, {.01, .003}},
            {10, SpectrumKind::source, 1., 7, {-.01, -.003}}};
        const std::vector<double> full{
            0., 1.2, .7, -.4, .2, -.1, .05, -.02, .01, -.003, .002, -.001};
        polynomial_case("eleventh-order-phases", 1e8, 75., 100, full, phased);
        polynomial_case("coherent-merge", 1e6, 50., 0, {0., 1., .5}, merged);
        polynomial_case("coherent-cancellation", 1e6, 50., 0, {0., 1., .5}, cancelled);
        polynomial_case("empty-polynomial", 1e6, 50., 0, {0.}, {});
        polynomial_case("uint64-max-linear",
                        1e6,
                        50.,
                        UINT64_MAX,
                        {0., 1.},
                        {{10, SpectrumKind::source, 1., UINT64_MAX, {.01, .003}}});
        const std::vector<double> nonlinear(full.begin() + 2, full.end());
        highorder_case("limited-eleventh-order-phases", 1e8, 75., 100, nonlinear, phased);
        highorder_case("limited-coherent-merge", 1e6, 50., 0, {.5, -.1}, merged);
        highorder_case("limited-coherent-cancellation", 1e6, 50., 0, {.5, -.1}, cancelled);
        highorder_case("direct-only", 1e6, 50., UINT64_MAX, {}, phased);
        highorder_case("empty-highorder", 1e6, 50., 0, {}, {});
        std::cout << "]\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
