// Installed public headers only. The probe instantiates the same inline API in
// a second translation unit; checks remain active in Release/NDEBUG builds.
#include <rfmodel/models/frequency_dependent_cable.hpp>
#include <rfmodel/noise/noise_matrix.hpp>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

bool frequency_dependent_cable_odr_probe();

namespace {
namespace rf = rfmodel;
std::size_t checks = 0;

void require(bool condition, const std::string &what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(what);
    }
}

void close(rf::Complex actual, rf::Complex expected, const std::string &what) {
    const double error = std::abs(actual - expected);
    require(std::isfinite(error) && error <= 2e-12 * (1. + std::abs(expected)), what);
}

template <class Exception, class Function>
void rejects(Function function, const std::string &what) {
    bool rejected = false;
    try {
        function();
    } catch (const Exception &) {
        rejected = true;
    }
    require(rejected, what);
}

// Independent moderate-loss ABCD oracle, deliberately not a reflection-factor
// rearrangement or a call through another RFModel transmission-line model.
rf::SMatrix abcd_oracle(double frequency_hz, const rf::FrequencyDependentCableParameters &p) {
    using LongComplex = std::complex<long double>;
    const long double ratio = static_cast<long double>(frequency_hz) / p.reference_frequency_hz;
    const long double loss =
        p.length_m * (p.sqrt_loss_db_per_m * std::sqrt(ratio) + p.linear_loss_db_per_m * ratio);
    const long double delay =
        static_cast<long double>(p.length_m) * std::sqrt(p.relative_permittivity) / 299792458.L;
    const LongComplex electrical_length{loss * std::log(10.L) / 20.L,
                                        2.L * std::acos(-1.L) * frequency_hz * delay};
    const LongComplex a = std::cosh(electrical_length);
    const LongComplex b = static_cast<long double>(p.characteristic_ohms) *
                          std::sinh(electrical_length) / static_cast<long double>(p.reference_ohms);
    const LongComplex c = static_cast<long double>(p.reference_ohms) *
                          std::sinh(electrical_length) /
                          static_cast<long double>(p.characteristic_ohms);
    const LongComplex denominator = 2.L * a + b + c;
    const auto cast = [](LongComplex value) {
        return rf::Complex{static_cast<double>(value.real()), static_cast<double>(value.imag())};
    };
    const auto reflection = cast((b - c) / denominator);
    const auto transmission = cast(2.L / denominator);
    return {2, {reflection, transmission, transmission, reflection}};
}

void check_thru(const rf::SMatrix &s, const std::string &what) {
    require(s.ports == 2 && s.values == std::vector<rf::Complex>{{}, 1., 1., {}}, what);
}

void check_public_api() {
    rf::FrequencyDependentCableParameters p;
    require(p.length_m == 1. && p.characteristic_ohms == 50. && p.relative_permittivity == 1. &&
                p.reference_frequency_hz == 1e9 && p.sqrt_loss_db_per_m == 0. &&
                p.linear_loss_db_per_m == 0. && p.reference_ohms == 50.,
            "installed aggregate defaults");
    p.length_m = 1.75;
    p.characteristic_ohms = 82.;
    p.relative_permittivity = 2.89;
    p.reference_frequency_hz = 2.5e9;
    p.sqrt_loss_db_per_m = .4;
    p.linear_loss_db_per_m = .125;
    p.reference_ohms = 37.;
    const rf::FrequencyDependentCableModel cable("installed cable", p);
    const rf::RFDeviceModel &device = cable;
    const rf::SParameterProvider &provider = cable;
    require(device.name() == "installed cable" && device.port_count() == 2,
            "public device interface");
    for (std::size_t port = 0; port < 2; ++port) {
        require(device.port(port).id == port &&
                    device.port(port).reference_impedance == rf::Complex{37., 0.},
                "public real port reference");
    }
    rejects<std::out_of_range>(
        [&] {
            device.port(2);
        },
        "port bounds remain checked");
    close(rf::cable_propagation_loss_db(p.reference_frequency_hz, p),
          1.75 * (.4 + .125),
          "independent contributions at caller reference frequency");
    close(rf::cable_propagation_loss_db(4. * p.reference_frequency_hz, p),
          1.75 * (2. * .4 + 4. * .125),
          "square-root and linear frequency scaling");

    for (double frequency : {0., 1e6, 1e8, 1e9, 2.5e9, 1e10}) {
        const auto direct = rf::frequency_dependent_cable_s_parameters(frequency, p);
        const auto model = provider.s_parameters(frequency);
        const auto expected = abcd_oracle(frequency, p);
        require(direct.ports == 2 && direct.values.size() == 4, "public matrix shape");
        for (std::size_t entry = 0; entry < 4; ++entry) {
            close(direct.values[entry], expected.values[entry], "independent complex ABCD oracle");
            close(model.values[entry], direct.values[entry], "wrapper equals public free function");
        }
        const auto noise = rf::passive_thermal_noise(direct, 290.).watts_per_hz;
        require(noise.ports == 2, "existing passive-noise composition");
        for (std::size_t i = 0; i < 2; ++i) {
            for (std::size_t j = 0; j < 2; ++j) {
                rf::Complex defect = i == j ? 1. : 0.;
                for (std::size_t k = 0; k < 2; ++k) {
                    defect -= direct(i, k) * std::conj(direct(j, k));
                }
                close(noise(i, j) / (1.380649e-23 * 290.), defect, "thermal-noise defect");
            }
        }
    }
    check_thru(provider.s_parameters(0.), "DC is exact thru despite impedance mismatch");
    p.length_m = 0.;
    check_thru(rf::frequency_dependent_cable_s_parameters(1e10, p), "zero length is exact thru");
    require(rf::cable_propagation_loss_db(1e10, p) == 0., "zero-length loss is exact zero");
}

void check_boundaries() {
    rf::FrequencyDependentCableParameters p;
    p.length_m = 2.;
    p.characteristic_ohms = 75.;
    p.sqrt_loss_db_per_m = std::numeric_limits<double>::max();
    const auto opaque = rf::frequency_dependent_cable_s_parameters(p.reference_frequency_hz, p);
    close(opaque(0, 0), .2, "opaque line reflection");
    close(opaque(1, 1), .2, "opaque line reverse reflection");
    require(opaque(0, 1) == rf::Complex{} && opaque(1, 0) == rf::Complex{},
            "opaque line transmission underflows to zero");
    rejects<std::overflow_error>(
        [&] {
            rf::cable_propagation_loss_db(p.reference_frequency_hz, p);
        },
        "finite loss helper rejects nonrepresentable total");
    rejects<std::invalid_argument>(
        [&] {
            rf::frequency_dependent_cable_s_parameters(-1., p);
        },
        "negative frequency rejected");
    p.relative_permittivity = 0.;
    rejects<std::invalid_argument>(
        [&] {
            rf::FrequencyDependentCableModel invalid("invalid", p);
        },
        "invalid material input rejected at construction");
    p = {};
    p.characteristic_ohms = std::numeric_limits<double>::max();
    rejects<std::invalid_argument>(
        [&] {
            rf::frequency_dependent_cable_s_parameters(0., p);
        },
        "unresolved impedance contrast rejected even at DC");
}
} // namespace

int main() {
    try {
        require(frequency_dependent_cable_odr_probe(), "second translation unit public API probe");
        check_public_api();
        check_boundaries();
        std::cout << "PASS: " << checks << " installed frequency-dependent cable checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "frequency-dependent cable consumer: " << error.what() << '\n';
        return 1;
    }
}
