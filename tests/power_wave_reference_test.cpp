#include "rfmodel/power_wave_reference.hpp"
#include "rfmodel/noise_renormalization.hpp"
#include "rfmodel/linear_analysis.hpp"
#include "test_support.hpp"
#include <iostream>

void matrix_near(const rfmodel::SMatrix &actual,
                 const rfmodel::SMatrix &expected,
                 double scale = 1.) {
    require(actual.ports == expected.ports, "matrix dimensions");
    for (std::size_t i = 0; i < actual.values.size(); ++i) {
        require(std::abs((actual.values[i] - expected.values[i]) / scale) < 2e-11,
                "power-wave matrix mismatch");
    }
}

int run() {
    using namespace rfmodel;
    const Complex load{37., -12.};
    for (Complex old_z : {Complex{50.}, Complex{75., 20.}}) {
        const SMatrix s{1, {(load - std::conj(old_z)) / (load + old_z)}};
        for (Complex new_z : {Complex{25.}, Complex{80., -17.}}) {
            const auto result = renormalize_power_waves(s, {old_z}, {new_z});
            near(result.scattering(0, 0), (load - std::conj(new_z)) / (load + new_z));
            near(s_to_z(result.scattering, std::vector<Complex>{new_z})(0, 0), load);
            near(s_to_y(result.scattering, std::vector<Complex>{new_z})(0, 0), 1. / load);
        }
    }
    const std::vector<Complex> refs{{25., 10.}, {100., -15.}};
    const SMatrix thru{2, {0., 1., 1., 0.}};
    const auto changed_thru = renormalize_power_waves(thru, {50., 50.}, refs);
    const Complex sum = refs[0] + refs[1];
    matrix_near(changed_thru.scattering,
                SMatrix{2,
                        {(refs[1] - std::conj(refs[0])) / sum,
                         100. / sum,
                         100. / sum,
                         (refs[0] - std::conj(refs[1])) / sum}});
    near(renormalize_power_waves(SMatrix{1, {1.}}, {50.}, {refs[0]}).scattering(0, 0), 1.);
    near(renormalize_power_waves(SMatrix{1, {-1.}}, {50.}, {refs[0]}).scattering(0, 0),
         -std::conj(refs[0]) / refs[0]);

    const SMatrix junction{
        3, {-1. / 3., 2. / 3., 2. / 3., 2. / 3., -1. / 3., 2. / 3., 2. / 3., 2. / 3., -1. / 3.}};
    const std::vector<Complex> junction_refs{{25., 10.}, {80., -5.}, {120., 15.}};
    const auto junction_result = renormalize_power_waves(junction, {50., 50., 50.}, junction_refs);
    Complex total_admittance{};
    for (auto z : junction_refs) {
        total_admittance += 1. / z;
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const auto z1 = junction_refs[row], z2 = junction_refs[column];
            Complex expected = 2. * std::sqrt(z1.real() * z2.real()) / (z1 * z2 * total_admittance);
            if (row == column) {
                expected -= std::conj(z1) / z1;
            }
            near(junction_result.scattering(row, column), expected);
        }
    }
    const SMatrix active{2, {Complex{.1, .05}, Complex{.03, -.02}, Complex{1.4, .2}, -.2}};
    const std::vector<Complex> old_refs{{40., -8.}, {60., 13.}};
    const auto transformed = renormalize_power_waves(active, old_refs, refs);
    matrix_near(renormalize_power_waves(transformed.scattering, refs, old_refs).scattering, active);
    matrix_near(s_to_z(active, old_refs), s_to_z(transformed.scattering, refs));
    matrix_near(s_to_y(active, old_refs), s_to_y(transformed.scattering, refs));
    // Independent voltage/current reconstruction also checks the noise transfer,
    // complex conjugates, port ordering, and noncommuting diagonal factors.
    for (const auto &noise : std::vector<std::vector<Complex>>{{0., 0.}, {{.2, -.1}, {.05, .3}}}) {
        const std::vector<Complex> a{{.4, -.2}, {-.1, .3}};
        std::vector<Complex> b(2), new_a(2), new_b(2);
        for (std::size_t row = 0; row < 2; ++row) {
            b[row] = noise[row];
            for (std::size_t column = 0; column < 2; ++column) {
                b[row] += active(row, column) * a[column];
            }
            const double root = std::sqrt(old_refs[row].real());
            const Complex voltage =
                (std::conj(old_refs[row]) * a[row] + old_refs[row] * b[row]) / root;
            const Complex current = (a[row] - b[row]) / root;
            const double new_root = std::sqrt(refs[row].real());
            new_a[row] = (voltage + refs[row] * current) / (2. * new_root);
            new_b[row] = (voltage - std::conj(refs[row]) * current) / (2. * new_root);
            near(std::norm(a[row]) - std::norm(b[row]), (voltage * std::conj(current)).real());
            near(std::norm(new_a[row]) - std::norm(new_b[row]),
                 std::norm(a[row]) - std::norm(b[row]));
        }
        for (std::size_t row = 0; row < 2; ++row) {
            Complex expected{};
            for (std::size_t column = 0; column < 2; ++column) {
                expected += transformed.scattering(row, column) * new_a[column] +
                            transformed.noise_transfer(row, column) * noise[column];
            }
            near(new_b[row], expected);
        }
    }
    const SMatrix passive{2, {Complex{.1, .05}, .4, .4, -.1}};
    constexpr double kt = 1.380649e-23 * 290.;
    const auto intrinsic = passive_thermal_noise(passive, 290.);
    const auto result = renormalize_power_waves(passive, {50., 50.}, refs);
    const auto noise = renormalize_noise(passive, intrinsic, std::vector<Complex>{50., 50.}, refs);
    matrix_near(
        noise.watts_per_hz, passive_thermal_noise(result.scattering, 290.).watts_per_hz, kt);
    matrix_near(renormalize_noise(result.scattering, noise, refs, std::vector<Complex>{50., 50.})
                    .watts_per_hz,
                intrinsic.watts_per_hz,
                kt);
    matrix_near(renormalize_power_waves(passive, {75., 75.}, {50., 50.}).scattering,
                renormalize_s(passive, 75., 50.));
    matrix_near(s_to_z(passive, std::vector<Complex>{50., 50.}), s_to_z(passive, 50.));
    matrix_near(s_to_y(passive, std::vector<Complex>{50., 50.}), s_to_y(passive, 50.));
    for (double reference : {1e-200, 1e200}) {
        const auto z = s_to_z(SMatrix{1, {0.}}, std::vector<Complex>{reference});
        const auto y = s_to_y(SMatrix{1, {0.}}, std::vector<Complex>{reference});
        require(std::abs(z(0, 0) / reference - 1.) < 1e-12, "large/small impedance scaling");
        require(std::abs(y(0, 0) * reference - 1.) < 1e-12, "large/small admittance scaling");
    }
    auto builder = [passive](double) {
        LinearNetwork network(50.);
        network.add(passive, 50.);
        return network;
    };
    const auto sweep = analyze_linear(
        FrequencyGrid{{1e9, 2e9}},
        {1, 0},
        builder,
        [intrinsic](double, const LinearNetwork &) {
            return intrinsic;
        },
        [refs](double f) {
            return f == 1e9 ? refs : std::vector<Complex>{75., 30.};
        });
    require(sweep.port_impedances_ohms[0] == refs, "frequency/selected-port output references");
    require(sweep.port_impedances_ohms[1] == std::vector<Complex>({75., 30.}),
            "sampled references");
    const SMatrix reordered{2, {-.1, .4, .4, Complex{.1, .05}}};
    matrix_near(sweep.scattering[0],
                renormalize_power_waves(reordered, {50., 50.}, refs).scattering);
    matrix_near(sweep.noise_correlation[0].watts_per_hz,
                passive_thermal_noise(sweep.scattering[0], 290.).watts_per_hz,
                kt);
    rejects<std::invalid_argument>([&] {
        renormalize_power_waves(active, {50.}, refs);
    });
    rejects<std::invalid_argument>([&] {
        renormalize_power_waves(active, {50., 50.}, {50., Complex{0., 10.}});
    });
    rejects<std::invalid_argument>([&] {
        renormalize_power_waves(active, {50., 50.}, {50., Complex{50., INFINITY}});
    });
    rejects<std::domain_error>([] {
        renormalize_power_waves(SMatrix{1, {5.}}, {50.}, {75.});
    });
    rejects<std::domain_error>([] {
        s_to_z(SMatrix{1, {1.}}, std::vector<Complex>{50.});
    });
    rejects<std::domain_error>([] {
        s_to_y(SMatrix{1, {-1.}}, std::vector<Complex>{50.});
    });
    rejects<std::runtime_error>([&] {
        analyze_linear(FrequencyGrid{{1e9}}, {0, 1}, builder, {}, [](double) {
            return std::vector<Complex>{50.};
        });
    });
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
