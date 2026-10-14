#include "rfmodel/io/touchstone_writer.hpp"
#include "../support/test_support.hpp"
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <iomanip>
#include <iostream>

using namespace rfmodel;

namespace {
TouchstoneData sample(std::size_t ports = 1) {
    TouchstoneData d;
    d.ports = ports;
    d.frequencies_hz = {0.};
    d.matrices = {SMatrix{ports, std::vector<Complex>(ports * ports)}};
    for (std::size_t i = 0; i < ports * ports; ++i) {
        d.matrices[0].values[i] = {static_cast<double>(2 * i + 1), -static_cast<double>(2 * i + 2)};
    }
    return d;
}

std::string serialize(const TouchstoneData &d) {
    std::ostringstream out;
    write_touchstone_v2(out, d);
    return out.str();
}

struct Buffer : std::stringbuf {
    int writes{}, syncs{};
    bool short_write{}, throwing{};

    std::streamsize xsputn(const char *s, std::streamsize n) override {
        ++writes;
        if (throwing) {
            throw std::runtime_error("streambuf failure");
        }
        return std::stringbuf::xsputn(s, short_write ? n / 2 : n);
    }

    int sync() override {
        ++syncs;
        return 0;
    }
};

struct Comma : std::numpunct<char> {
    char do_decimal_point() const override {
        return ',';
    }

    char do_thousands_sep() const override {
        return '.';
    }

    std::string do_grouping() const override {
        return "\3";
    }
};

void invalid(const TouchstoneData &d) {
    Buffer buffer;
    std::ostream out(&buffer);
    rejects<std::invalid_argument>([&] {
        write_touchstone_v2(out, d);
    });
    require(buffer.writes == 0 && out.good(), "invalid input touched output");
}

void goldens() {
    // Hand-written independent layout oracle from IBIS 2.0 pp.6-17, not reader output.
    require(serialize(sample()) ==
                "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 1\n"
                "[Number of Frequencies] 1\n[Reference] 50\n[Matrix Format] Full\n"
                "[Network Data]\n0 1 -2\n[End]\n",
            "one port golden");
    auto d = sample(2);
    d.reference_impedance_ohms = 75;
    d.reference_impedances_ohms = {31, 73};
    const std::string expected = "[Version] 2.0\n# Hz S RI R 75\n[Number of Ports] 2\n"
                                 "[Two-Port Data Order] 12_21\n[Number of Frequencies] 1\n"
                                 "[Reference] 31 73\n[Matrix Format] Full\n[Network Data]\n"
                                 "0 1 -2 3 -4 5 -6 7 -8\n[End]\n";
    require(serialize(d) == expected, "nonreciprocal two port golden");
    for (auto format :
         {TouchstoneFormat::MA, TouchstoneFormat::DB, static_cast<TouchstoneFormat>(99)}) {
        for (auto matrix : {TouchstoneMatrixFormat::Lower, TouchstoneMatrixFormat::Upper}) {
            d.format = format;
            d.matrix_format = matrix;
            d.version = static_cast<TouchstoneVersion>(99);
            d.two_port_order = static_cast<TouchstoneTwoPortOrder>(99);
            d.frequency_scale = std::numeric_limits<double>::quiet_NaN();
            require(serialize(d) == expected, "representation metadata lost matrix elements");
        }
    }
    require(serialize(sample(3)) ==
                "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 3\n"
                "[Number of Frequencies] 1\n[Reference] 50 50 50\n[Matrix Format] Full\n"
                "[Network Data]\n0 1 -2 3 -4 5 -6\n 7 -8 9 -10 11 -12\n"
                " 13 -14 15 -16 17 -18\n[End]\n",
            "asymmetric three port golden");
}

void imported_representations() {
    const char *path = "touchstone-writer-source.ts";
    for (const auto *format : {"MA", "DB"}) {
        for (const auto *matrix : {"Lower", "Upper"}) {
            {
                std::ofstream file(path);
                file << "[Version] 2.0\n# GHz S " << format << " R 75\n"
                     << "[Number of Ports] 2\n[Two-Port Data Order] 21_12\n"
                     << "[Number of Frequencies] 1\n[Matrix Format] " << matrix
                     << "\n[Network Data]\n1 "
                     << (std::string(format) == "MA" ? "1 0 10 0 100 0" : "0 0 20 0 40 0")
                     << "\n[End]\n";
            }
            const auto data = read_touchstone(path);
            require(serialize(data) == "[Version] 2.0\n# Hz S RI R 75\n[Number of Ports] 2\n"
                                       "[Two-Port Data Order] 12_21\n[Number of Frequencies] 1\n"
                                       "[Reference] 75 75\n[Matrix Format] Full\n[Network Data]\n"
                                       "1000000000 1 0 10 0 10 0 100 0\n[End]\n",
                    "parsed MA/DB triangular normalization");
        }
    }
    std::remove(path);
}

void validation() {
    auto test = [](auto mutate) {
        auto d = sample(2);
        mutate(d);
        invalid(d);
    };
    test([](auto &d) {
        d.ports = 0;
    });
    test([](auto &d) {
        d.ports = 1025;
    });
    test([](auto &d) {
        d.ports = (std::numeric_limits<std::size_t>::max)();
    });
    test([](auto &d) {
        d.frequencies_hz.clear();
    });
    test([](auto &d) {
        d.matrices.clear();
    });
    test([](auto &d) {
        d.matrices[0].ports = 1;
    });
    test([](auto &d) {
        d.matrices[0].values.pop_back();
    });
    test([](auto &d) {
        d.matrices[0].values.push_back({});
    });
    test([](auto &d) {
        d.frequencies_hz.push_back(0.);
        d.matrices.push_back(d.matrices[0]);
    });
    test([](auto &d) {
        d.frequencies_hz = {2., 1.};
        d.matrices.push_back(d.matrices[0]);
    });
    test([](auto &d) {
        d.reference_impedances_ohms = {50};
    });
    test([](auto &d) {
        d.reference_impedances_ohms = {50, 50, 50};
    });
    test([](auto &d) {
        d.noise_samples.push_back({});
    });
    for (double bad : {-1.,
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        test([&](auto &d) {
            d.frequencies_hz[0] = bad;
        });
        test([&](auto &d) {
            d.reference_impedance_ohms = bad;
        });
        test([&](auto &d) {
            d.reference_impedances_ohms = {50, bad};
        });
        if (!std::isfinite(bad)) {
            test([&](auto &d) {
                d.matrices[0].values.back() = {bad, 0};
            });
            test([&](auto &d) {
                d.matrices[0].values.back() = {0, bad};
            });
        }
    }
    test([](auto &d) {
        d.reference_impedance_ohms = 0;
    });
    test([](auto &d) {
        d.reference_impedances_ohms = {0, 50};
    });
}

bool identical(double a, double b) {
    return a == b && std::signbit(a) == std::signbit(b);
}

void precision() {
    const auto max = (std::numeric_limits<double>::max)();
    auto d = sample();
    d.frequencies_hz = {-0.,
                        std::numeric_limits<double>::denorm_min(),
                        std::numeric_limits<double>::min(),
                        1.,
                        std::nextafter(1., 2.),
                        max};
    d.matrices.clear();
    for (double x : d.frequencies_hz) {
        d.matrices.push_back({1, {{x, -x}}});
    }
    d.reference_impedance_ohms = std::numeric_limits<double>::denorm_min();
    d.reference_impedances_ohms = {max};
    const auto text = serialize(d);
    // Independent C conversion oracle: this test never changes the initial C locale.
    // strtod may set ERANGE for a representable subnormal; compare the returned
    // value against the independent expected doubles instead of discarding it.
    require(std::string(std::localeconv()->decimal_point) == ".", "oracle C locale");
    std::istringstream values(text.substr(text.find("[Network Data]\n") + 15));
    values.imbue(std::locale::classic());
    for (std::size_t i = 0; i < d.frequencies_hz.size(); ++i) {
        double f{}, re{}, im{};
        for (double *value : {&f, &re, &im}) {
            std::string token;
            require(static_cast<bool>(values >> token), "extreme numeric extraction");
            char *end = nullptr;
            *value = std::strtod(token.c_str(), &end);
            require(end == token.c_str() + token.size() && std::isfinite(*value),
                    "independent decimal conversion");
        }
        require(identical(f, d.frequencies_hz[i]) &&
                    identical(re, d.matrices[i].values[0].real()) &&
                    identical(im, d.matrices[i].values[0].imag()),
                "extreme precision loss");
    }
    const char *path = "touchstone-writer-extremes.ts";
    {
        std::ofstream file(path);
        file << text;
    }
    const auto roundtrip = read_touchstone(path);
    std::remove(path);
    require(roundtrip.reference_impedance_ohms == d.reference_impedance_ohms &&
                roundtrip.reference_impedances_ohms == d.reference_impedances_ohms,
            "reference precision");
    for (std::size_t i = 0; i < d.frequencies_hz.size(); ++i) {
        require(
            identical(roundtrip.frequencies_hz[i], d.frequencies_hz[i]) &&
                identical(roundtrip.matrices[i].values[0].real(), d.matrices[i].values[0].real()) &&
                identical(roundtrip.matrices[i].values[0].imag(), d.matrices[i].values[0].imag()),
            "extreme reader roundtrip");
    }
}

void unchanged_input() {
    auto data = sample(2);
    data.format = TouchstoneFormat::DB;
    data.version = TouchstoneVersion::V2_0;
    data.matrix_format = TouchstoneMatrixFormat::Lower;
    data.frequency_scale = std::numeric_limits<double>::quiet_NaN();
    data.reference_impedance_ohms = 75.;
    data.reference_impedances_ohms = {31., 73.};
    const auto snapshot = data;
    auto verify = [&] {
        require(data.ports == snapshot.ports && std::isnan(data.frequency_scale) &&
                    data.format == snapshot.format && data.version == snapshot.version &&
                    data.matrix_format == snapshot.matrix_format &&
                    data.two_port_order == snapshot.two_port_order &&
                    data.reference_impedance_ohms == snapshot.reference_impedance_ohms &&
                    data.reference_impedances_ohms == snapshot.reference_impedances_ohms &&
                    data.frequencies_hz == snapshot.frequencies_hz &&
                    data.matrices.size() == snapshot.matrices.size() && data.noise_samples.empty(),
                "input metadata mutated");
        for (std::size_t i = 0; i < data.matrices.size(); ++i) {
            require(data.matrices[i].ports == snapshot.matrices[i].ports &&
                        data.matrices[i].values == snapshot.matrices[i].values,
                    "input matrix mutated");
        }
    };
    serialize(data);
    verify();
    Buffer buffer;
    buffer.short_write = true;
    std::ostream out(&buffer);
    rejects<std::ios_base::failure>([&] {
        write_touchstone_v2(out, data);
    });
    verify();
}

void streams() {
    for (const auto mask : {std::ios::goodbit,
                            std::ios::badbit,
                            std::ios::failbit,
                            std::ios::eofbit,
                            std::ios::badbit | std::ios::failbit,
                            std::ios::badbit | std::ios::eofbit,
                            std::ios::failbit | std::ios::eofbit,
                            std::ios::badbit | std::ios::failbit | std::ios::eofbit}) {
        Buffer buffer, tied_buffer;
        std::ostream out(&buffer), tied(&tied_buffer);
        out.imbue(std::locale(std::locale::classic(), new Comma));
        out << std::hex << std::showpos << std::scientific << std::setprecision(3)
            << std::setfill('@');
        out.width(91);
        out.setf(std::ios::unitbuf);
        out.tie(&tied);
        out.exceptions(mask);
        auto locale = out.getloc();
        auto flags = out.flags();
        auto verify = [&] {
            require(out.getloc() == locale && out.flags() == flags && out.precision() == 3 &&
                        out.fill() == '@' && out.width() == 91 && out.exceptions() == mask &&
                        out.tie() == &tied && !buffer.syncs && !tied_buffer.syncs,
                    "caller stream state changed");
        };
        write_touchstone_v2(out, sample());
        require(buffer.str() == serialize(sample()) && buffer.writes == 1,
                "not one canonical write");
        verify();
        buffer.short_write = true;
        rejects<std::ios_base::failure>([&] {
            write_touchstone_v2(out, sample());
        });
        require(out.bad(), "short write missing badbit");
        verify();
        out.clear();
        buffer.short_write = false;
        buffer.throwing = true;
        rejects<std::ios_base::failure>([&] {
            write_touchstone_v2(out, sample());
        });
        require(out.bad(), "throwing buffer missing badbit");
        verify();
        for (auto state : {std::ios::eofbit, std::ios::failbit, std::ios::badbit}) {
            out.exceptions(std::ios::goodbit);
            out.clear(state);
            try {
                out.exceptions(mask);
            } catch (const std::ios_base::failure &) {
            }
            const auto writes = buffer.writes;
            rejects<std::ios_base::failure>([&] {
                write_touchstone_v2(out, sample());
            });
            require(out.rdstate() == state && writes == buffer.writes,
                    "initial failure touched output");
            verify();
        }
    }
    std::ostream null(nullptr);
    rejects<std::ios_base::failure>([&] {
        write_touchstone_v2(null, sample());
    });
}

void budget() {
    {
        touchstone_writer_detail::Stage exact;
        const std::string chunk(1024, 'x');
        for (std::size_t n = 0; n < touchstone_v2_max_output_bytes / chunk.size(); ++n) {
            require(exact.sputn(chunk.data(), static_cast<std::streamsize>(chunk.size())) == 1024,
                    "stage short write before budget");
        }
        require(exact.size() == touchstone_v2_max_output_bytes, "exact budget not accepted");
        rejects<std::length_error>([&] {
            exact.sputc('x');
        });
        require(exact.size() == touchstone_v2_max_output_bytes, "overflow changed stage");
    }
    auto d = sample(1024);
    std::fill(d.matrices[0].values.begin(), d.matrices[0].values.end(), Complex{1., 0.});
    // Upper accepted port boundary with a small representation.
    require(serialize(d).find("[Number of Ports] 1024\n") != std::string::npos,
            "1024 ports rejected");
    d.matrices.push_back(d.matrices.front());
    d.frequencies_hz.push_back(1.);
    for (auto &m : d.matrices) {
        std::fill(m.values.begin(),
                  m.values.end(),
                  Complex{1.2345678901234567e200, -1.2345678901234567e200});
    }
    Buffer buffer;
    std::ostream out(&buffer);
    rejects<std::length_error>([&] {
        write_touchstone_v2(out, d);
    });
    require(buffer.writes == 0 && out.good(), "budget failure touched output");
}
} // namespace

int main() {
    goldens();
    imported_representations();
    validation();
    precision();
    unchanged_input();
    streams();
    budget();
    std::cout << "Touchstone writer goldens, validation, precision, streams and budget passed\n";
}
