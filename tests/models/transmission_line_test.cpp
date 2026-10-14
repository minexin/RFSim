#include "rfmodel/models/transmission_line.hpp"
#include "rfmodel/linear/network.hpp"
#include "rfmodel/noise/noise_matrix.hpp"
#include "../fixtures/transmission_line_oracle.hpp"
#include <iostream>
#include "../support/test_support.hpp"
#include <limits>

namespace {
void oracle_close(double actual, double expected, std::size_t row, const char *component) {
    const double tolerance =
        std::max(4. * std::numeric_limits<double>::denorm_min(), 3e-14 * std::abs(expected));
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr.precision(17);
        std::cerr << "oracle row " << row << " " << component << ": " << actual << " expected "
                  << expected << " tolerance " << tolerance << '\n';
        throw std::runtime_error("independent ABCD oracle mismatch");
    }
}
} // namespace

int main() {
    using namespace rfmodel;
    TransmissionLineModel line("100 ohm line", 100., 0.25e-9);
    const auto quarter = line.s_parameters(1e9);
    near(quarter(0, 0), 0.6);
    near(quarter(1, 0), Complex{0., -0.8});
    near(line.s_parameters(2e9)(1, 0), -1.);
    near(line.s_parameters(2e9)(0, 0), 0.);
    near(line.s_parameters(0.)(1, 0), 1.);
    near(line.port(1).reference_impedance, 50.);
    for (double frequency : {0., 1e6, 1e9, 1.3e9, 2e9}) {
        const auto s = line.s_parameters(frequency);
        near(std::norm(s(0, 0)) + std::norm(s(1, 0)), 1.);
        near(s(0, 0) * std::conj(s(0, 1)) + s(1, 0) * std::conj(s(1, 1)), 0.);
        TransmissionLineModel matched("matched", 75., 0.25e-9, 3., 75.);
        MatchedTransmissionModel existing("existing", 3., 0.25e-9, 75.);
        near(matched.s_parameters(frequency)(1, 0), existing.s_parameters(frequency)(1, 0));
        near(matched.s_parameters(frequency)(0, 0), 0.);
        // Two uniform sections must equal a single section of double length/loss.
        TransmissionLineModel lossy("section", 100., 0.25e-9, 2.);
        TransmissionLineModel doubled("double", 100., 0.5e-9, 4.);
        LinearNetwork network;
        network.add(lossy.s_parameters(frequency));
        network.add(lossy.s_parameters(frequency));
        network.connect(1, 2);
        const auto combined = network.external_s({0, 3});
        const auto expected = doubled.s_parameters(frequency);
        for (std::size_t i = 0; i < 2; ++i) {
            for (std::size_t j = 0; j < 2; ++j) {
                near(combined(i, j), expected(i, j));
            }
        }
    }
    std::size_t oracle_row = 0;
    for (const auto &entry : transmission_line_oracle) {
        TransmissionLineModel model("oracle", entry.zc, entry.delay, entry.loss, entry.z0);
        const auto s = model.s_parameters(entry.frequency);
        oracle_close(s(0, 0).real(), entry.reflection_real, oracle_row, "S11 real");
        oracle_close(s(0, 0).imag(), entry.reflection_imag, oracle_row, "S11 imaginary");
        oracle_close(s(1, 0).real(), entry.transmission_real, oracle_row, "S21 real");
        oracle_close(s(1, 0).imag(), entry.transmission_imag, oracle_row, "S21 imaginary");
        require(s(0, 0) == s(1, 1) && s(0, 1) == s(1, 0), "reciprocal symmetric line");
        // The singular values of this symmetric two-port are |S11 +/- S21|.
        require(std::abs(s(0, 0) + s(1, 0)) <= 1. + 2e-14 &&
                    std::abs(s(0, 0) - s(1, 0)) <= 1. + 2e-14,
                "line passivity");
        const auto noise = passive_thermal_noise(s, 290.).watts_per_hz;
        require(noise.ports == 2 && std::isfinite(noise(0, 0).real()), "passive noise interface");
        ++oracle_row;
    }
    for (const double impedance : {1e15, 2.5e-12}) {
        TransmissionLineModel resonant("exact", impedance, 0.5);
        for (const double frequency : {0., 1., 2., 3., 4.}) {
            const auto s = resonant.s_parameters(frequency);
            require(s(0, 0) == Complex{}, "exact half-wave reflection");
            require(s(1, 0) == Complex{std::remainder(frequency, 2.) == 0. ? 1. : -1., 0.},
                    "exact half-wave transmission");
        }
    }
    for (double impedance : {1e15, 2.5e-12}) {
        for (double delay : {0., 0.5, std::nextafter(0.5, 1.)}) {
            TransmissionLineModel section("tiny loss", impedance, delay, 1e-16);
            TransmissionLineModel doubled("double tiny loss", impedance, 2. * delay, 2e-16);
            LinearNetwork cascade;
            cascade.add(section.s_parameters(1.));
            cascade.add(section.s_parameters(1.));
            cascade.connect(1, 2);
            const auto combined = cascade.external_s({0, 3});
            const auto expected = doubled.s_parameters(1.);
            for (std::size_t row = 0; row < 2; ++row) {
                for (std::size_t column = 0; column < 2; ++column) {
                    near(combined(row, column), expected(row, column));
                }
            }
        }
    }
    const double minimum = std::numeric_limits<double>::denorm_min();
    TransmissionLineModel subnormal_matched("subnormal phase", 50., 0.5);
    require(subnormal_matched.s_parameters(minimum)(1, 0).imag() == -3. * minimum,
            "matched phase survives an underflowed cycle product");
    TransmissionLineModel subnormal_loss("subnormal loss", 1e15, 0., minimum);
    require(subnormal_loss.s_parameters(0.)(0, 0).real() > 0.,
            "subnormal loss survives impedance amplification");
    // Even a fully opaque line retains the existing phase-product validation.
    rejects<std::overflow_error>([] {
        TransmissionLineModel enormous("enormous", 50., std::numeric_limits<double>::max(), 10000.);
        enormous.s_parameters(2.);
    });
    TransmissionLineModel opaque("opaque", 100., 1., 10000.);
    near(opaque.s_parameters(1.)(0, 0), 1. / 3.);
    near(opaque.s_parameters(1.)(1, 0), 0.);
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 0., 0.);
    });
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 100., -1.);
    });
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 100., 0., -1.);
    });
    rejects<std::invalid_argument>([] {
        TransmissionLineModel bad("bad", 1e300, 0.);
    });
    rejects<std::invalid_argument>([&] {
        line.s_parameters(-1.);
    });
    rejects<std::invalid_argument>([&] {
        line.s_parameters(std::numeric_limits<double>::infinity());
    });
    rejects<std::out_of_range>([&] {
        line.port(2);
    });
}
