#include "rfmodel/core/interpolation.hpp"
#include "rfmodel/core/linear_solver.hpp"
#include "../support/test_support.hpp"
#include <limits>
#include <type_traits>

using namespace rfmodel;

static_assert(std::is_same<decltype(&evaluate_two_port),
                           TwoPortWave (*)(const SMatrix &, Complex, Complex)>::value,
              "two-port function type is unchanged");

void indices() {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    for (auto ports : {maximum / 2 + 1, maximum}) {
        SMatrix s{ports, {42.0}};
        const SMatrix &cs = s;
        const auto row = ports == maximum ? 1u : 2u;
        const auto column = ports == maximum ? 1u : 0u;
        rejects<std::out_of_range>([&] {
            s.index(row, column);
        });
        rejects<std::out_of_range>([&] {
            s(row, column) = 0;
        });
        rejects<std::out_of_range>([&] {
            (void)cs(row, column);
        });
        require(s(0, 0) == Complex{42}, "failed index changed matrix");
    }
    SMatrix s{2, {1}};
    const SMatrix &cs = s;
    for (auto index : {std::size_t{2}, maximum}) {
        rejects<std::out_of_range>([&] {
            (void)s(index, 0);
        });
        rejects<std::out_of_range>([&] {
            (void)cs(0, index);
        });
    }
    rejects<std::out_of_range>([&] {
        (void)s(1, 1);
    });
    rejects<std::out_of_range>([&] {
        (void)cs(1, 1);
    });
    require(s.index(1, 1) == 3, "index must not require backing storage");
    rejects<std::out_of_range>([] {
        SMatrix{}.index(0, 0);
    });
    // No arbitrary port cap: valid arithmetic and available storage remain usable.
    SMatrix large{1025, std::vector<Complex>(1025 * 1025)};
    large(1024, 1024) = 7;
    require(large(1024, 1024) == Complex{7}, "large matrix indexing");
}

void two_port() {
    const SMatrix valid{2, {{1, 2}, {3, -1}, {-4, 2}, {.5, -.5}}};
    const Complex a{2, -3}, b{-1, 4};
    auto check_valid = [&] {
        const auto wave = evaluate_two_port(valid, a, b);
        near(wave.a1, a);
        near(wave.a2, b);
        near(wave.b1, valid(0, 0) * a + valid(0, 1) * b);
        near(wave.b2, valid(1, 0) * a + valid(1, 1) * b);
    };
    for (std::size_t count : {0u, 1u, 3u, 5u}) {
        rejects<std::invalid_argument>([&] {
            evaluate_two_port(SMatrix{2, std::vector<Complex>(count)}, a);
        });
        check_valid();
    }
    rejects<std::invalid_argument>([&] {
        evaluate_two_port(SMatrix{1, {1, 2, 3, 4}}, a);
    });
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        for (bool imaginary : {false, true}) {
            const Complex bad = imaginary ? Complex{0, invalid} : Complex{invalid, 0};
            for (std::size_t i = 0; i < 4; ++i) {
                auto s = valid;
                s.values[i] = bad;
                rejects<std::invalid_argument>([&] {
                    evaluate_two_port(s, a, b);
                });
                check_valid();
            }
            rejects<std::invalid_argument>([&] {
                evaluate_two_port(valid, bad, b);
            });
            rejects<std::invalid_argument>([&] {
                evaluate_two_port(valid, a, bad);
            });
            check_valid();
        }
    }
    const double max = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < 4; ++i) {
        SMatrix s{2, std::vector<Complex>(4)};
        s.values[i] = max;
        rejects<std::overflow_error>([&] {
            evaluate_two_port(s, 2, 2);
        });
        check_valid();
    }
    rejects<std::overflow_error>([&] {
        evaluate_two_port(SMatrix{2, {max, max, 0, 0}}, 1, 1);
    });
    const auto zero = evaluate_two_port(valid, 0, 0);
    require(zero.b1 == Complex{} && zero.b2 == Complex{}, "zero excitation");
    const auto gain = evaluate_two_port(SMatrix{2, {0, 0, 10, 0}}, 1);
    near(gain.b2, 10);
    check_valid();
}

void static_model() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<PortInfo> ports{{99, "", {nan, inf}}, {99, "", {-1, 0}}};
    const SMatrix valid{2, {1, 2, 3, 4}};
    StaticSParameterModel model("", ports, valid);
    for (double f : {nan, inf, -inf, -1.0, 0.0, 1.0}) {
        require(model.s_parameters(f).values == valid.values, "static frequency ignored");
    }
    require(model.name().empty() && model.port(0).id == 99 &&
                std::isnan(model.port(0).reference_impedance.real()),
            "metadata preserved");
    require(StaticSParameterModel("", {}, {}).port_count() == 0, "zero ports preserved");
    for (double bad : {nan, inf, -inf}) {
        for (bool imaginary : {false, true}) {
            for (std::size_t i = 0; i < 4; ++i) {
                auto s = valid;
                s.values[i] = imaginary ? Complex{0, bad} : Complex{bad, 0};
                rejects<std::invalid_argument>([&] {
                    StaticSParameterModel("", ports, s);
                });
                require(model.s_parameters(0).values == valid.values, "failure isolation");
            }
        }
    }
    for (auto s : {SMatrix{1, {1}},
                   SMatrix{2, {1, 2, 3}},
                   SMatrix{2, {1, 2, 3, 4, 5}},
                   SMatrix{std::numeric_limits<std::size_t>::max() / 2 + 1, {}}}) {
        rejects<std::invalid_argument>([&] {
            StaticSParameterModel("", ports, s);
        });
    }
    const std::size_t n = 1025;
    require(
        StaticSParameterModel("", std::vector<PortInfo>(n), SMatrix{n, std::vector<Complex>(n * n)})
                .port_count() == n,
        "static model has no new port cap");
}

void interpolation_policy() {
    TouchstoneData d;
    d.ports = 1;
    d.frequencies_hz = {1, 3};
    d.matrices = {{1, {2}}, {1, {4}}};
    for (int invalid : {-1, 2, 12345}) {
        for (double f : {0., 1., 2., 3., 4.}) {
            rejects<std::invalid_argument>([&] {
                interpolate_s(d, f, static_cast<OutOfBand>(invalid));
            });
            near(interpolate_s(d, 2)(0, 0), 3);
        }
    }
    rejects<std::out_of_range>([&] {
        interpolate_s(d, 0);
    });
    near(interpolate_s(d, 0, OutOfBand::Clamp)(0, 0), 2);
    near(interpolate_s(d, 4, OutOfBand::Clamp)(0, 0), 4);
    d.frequencies_hz.resize(1);
    d.matrices.resize(1);
    for (double f : {0., 1., 2.}) {
        rejects<std::invalid_argument>([&] {
            interpolate_s(d, f, static_cast<OutOfBand>(9));
        });
        near(interpolate_s(d, 1)(0, 0), 2);
    }
}

int main() {
    indices();
    two_port();
    static_model();
    interpolation_policy();
}
