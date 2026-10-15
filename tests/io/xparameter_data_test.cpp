#include "rfmodel/io/xparameter_data.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <sstream>

namespace {
using namespace rfmodel;
std::size_t rejected = 0;

XParameterDocument parse(const std::string &s, XParameterLimits limits = {}) {
    std::istringstream in(s);
    return read_xparameter_data(in, limits);
}

const std::string simple = "BEGIN PHDModelData\n% x(real)\n1\nEND\n";
const std::string attributes = "BEGIN XParamAttributes\n% Index(int) Version(real) NumPorts(int) "
                               "NumFundFreqs(int)\n0 2.0 3 2\nEND\n";

void reject(const std::string &s, XParameterLimits limits = {}) {
    try {
        (void)parse(s, limits);
    } catch (const XParameterParseError &e) {
        require(e.location().line > 0 && e.location().column > 0, "positive diagnostic location");
        require(std::string(e.what()).find("line ") != std::string::npos, "located diagnostic");
        ++rejected;
        return;
    }
    throw std::runtime_error("unexpected acceptance: " + s);
}

std::string scalar(const char *type, const std::string &token) {
    return std::string("BEGIN PHDModelData\n% value(") + type + ")\n" + token + "\nEND\n";
}

const XParameterValue &at(const XParameterDocument &d,
                          std::size_t b = 0,
                          std::size_t r = 0,
                          std::size_t g = 0,
                          std::size_t c = 0) {
    return d.blocks().at(b).rows.at(r).groups.at(g).values.at(c);
}

void structural() {
    const auto bom = parse(std::string("\xef\xbb\xbf") + simple);
    require(bom.blocks()[0].location.column == 4, "BOM byte offset retained");
    reject(simple + std::string("\xef\xbb\xbf") + simple);
    const auto d = parse("! autonomous synthetic fixture, not a vendor file\r\n"
                         "REM preserved values, no wave interpretation\r\n"
                         "VAR fund_1(real) = 1e9\r\n"
                         "BEGIN PHDModelData\r\n# z0(real) = 50\r\n"
                         "% AN_1_1(real) Label(string)\r\n% FB_2_1(complex) Index(int)\r\n"
                         "0.001 \"RF ! port\"\r\n! comment between physical groups\r\n"
                         "-2.5 3.5 -7\r\n0.002 'second port'\r\n0 -0 +8\r\nEND\r\n"
                         "VAR fund_1(real) = 2e9\r\nBEGIN PHDModelData\r\n% x(real)\r\n3\r\nEND");
    require(d.version() == XParameterVersion::Legacy && d.blocks().size() == 2,
            "legacy and blocks");
    const auto &b = d.blocks()[0];
    require(b.headers.size() == 2 && b.rows.size() == 2 && b.rows[0].groups.size() == 2,
            "physical groups become rows");
    require(b.headers[0].fields[1].type == XParameterType::String, "typed declaration");
    require(std::get<std::string>(at(d, 0, 0, 0, 1).value) == "RF ! port", "quoted string content");
    require(at(d, 0, 0, 0, 1).raw_tokens[0] == "\"RF ! port\"", "quoted raw token");
    require(at(d, 0, 0, 1, 0).raw_tokens == std::vector<std::string>({"-2.5", "3.5"}),
            "RI raw token order");
    require(std::get<std::complex<double>>(at(d, 0, 0, 1, 0).value) ==
                std::complex<double>(-2.5, 3.5),
            "RI value");
    require(std::get<std::int64_t>(at(d, 0, 0, 1, 1).value) == -7, "integer value");
    require(b.location.line == 4 && b.constants[0].field.location.line == 5,
            "declaration locations");
    require(at(d, 0, 0, 0, 0).location.line == 8 && at(d, 0, 0, 0, 1).location.column == 7,
            "value locations");
    require(std::get<double>(b.variables[0].value.value) == 1e9 &&
                std::get<double>(d.blocks()[1].variables[0].value.value) == 2e9,
            "VAR binds next block only");
    require(d.blocks()[1].constants.empty(), "constants not inherited");
    const auto no_inherit = parse("VAR pump(real) = 7\n" + simple + simple);
    require(no_inherit.blocks()[0].variables.size() == 1 &&
                no_inherit.blocks()[1].variables.empty(),
            "VAR not inherited without redeclaration");

    const auto v = parse(attributes +
                         "BEGIN XParamPortData\n% PortNumber(int) RefZ0(complex) PortName(string)\n"
                         "1 50 0 Undefined\n2 75 -2 \"Output port\"\nEND\n"
                         "VAR fund_1(real) = 1e9\nVAR fund_2(real) = 1.2e9\nVAR VDC_3(real) = 10\n"
                         "BEGIN XParamData\n% FI_3(real) FB_2_1_m2(complex)\n% XY_3_0_0(complex)\n"
                         "0.0133849 -2.06795e-7 9.04205e-7\n-3 4\nEND\n");
    require(v.version() == XParameterVersion::V2_0 && v.blocks().size() == 3, "version 2 metadata");
    require(v.blocks()[1].rows.size() == 2 && v.blocks()[2].variables.size() == 3,
            "port and outer declarations");
    require(v.blocks()[2].headers[0].fields[1].name == "FB_2_1_m2" &&
                v.blocks()[2].headers[1].fields[0].name == "XY_3_0_0",
            "negative mixing/DC names remain opaque");
    require(std::get<double>(at(v, 2).value) == 0.0133849, "raw FI value unchanged");
    require(std::get<std::complex<double>>(at(v, 2, 0, 1).value) == std::complex<double>(-3, 4),
            "raw XY unchanged");
}

void numeric() {
    require(std::get<std::int64_t>(at(parse(scalar("int", "9223372036854775807"))).value) ==
                std::numeric_limits<std::int64_t>::max(),
            "int64 max");
    require(std::get<std::int64_t>(at(parse(scalar("int", "-9223372036854775808"))).value) ==
                std::numeric_limits<std::int64_t>::min(),
            "int64 min");
    for (const auto &s : {"9223372036854775808",
                          "-9223372036854775809",
                          "9999999999999999999999999",
                          "+",
                          "-",
                          "1.0",
                          "1e0",
                          "0x10",
                          "1junk",
                          "\"1\""}) {
        reject(scalar("int", s));
    }
    for (const auto &s : {"nan",
                          "NaN",
                          "inf",
                          "-Inf",
                          "1e9999",
                          "1e-9999",
                          "0x1p0",
                          "1,25",
                          "1D2",
                          "1f",
                          ".",
                          "+",
                          "1e",
                          "1e+",
                          "1.2junk",
                          "\"1.2\""}) {
        reject(scalar("real", s));
    }
    for (const auto &s : {"1",
                          "+1.",
                          ".5",
                          "-.25",
                          "1E+2",
                          "-0",
                          "0e-9999",
                          "2.2250738585072014e-308",
                          "4.9406564584124654e-324"}) {
        (void)parse(scalar("real", s));
    }
    require(std::signbit(std::get<double>(at(parse(scalar("real", "-0"))).value)),
            "negative zero preserved");
    require(std::get<double>(at(parse(scalar("real", "4.9406564584124654e-324"))).value) ==
                std::numeric_limits<double>::denorm_min(),
            "representable subnormal preserved");
    reject(scalar("complex", "1 inf"));
    reject(scalar("complex", "nan 1"));

    struct Comma : std::numpunct<char> {
        char do_decimal_point() const override {
            return ',';
        }
    };

    struct LocaleGuard {
        std::locale old{std::locale()};

        ~LocaleGuard() {
            std::locale::global(old);
        }
    } guard;

    std::locale::global(std::locale(std::locale::classic(), new Comma));
    require(std::get<double>(at(parse(scalar("real", "1.25"))).value) == 1.25,
            "classic locale independent of global locale");
    reject(scalar("real", "1,25"));
}

void malformed() {
    for (const auto &s : {"",
                          "! comment only\n",
                          "BEGIN Unknown\n% x(real)\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(float)\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(real) x(real)\n1 2\nEND\n",
                          "VAR x(real) = 1\nVAR x(real) = 2\n",
                          "VAR x(real) = 1\nBEGIN PHDModelData\n% x(real)\n1\nEND\n",
                          "BEGIN PHDModelData\n# x(real) = 1\n% x(real)\n2\nEND\n",
                          "BEGIN PHDModelData\n%\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(real)y(real)\n1 2\nEND\n",
                          "BEGIN PHDModelData\n% x real\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(real\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n1 2\nEND\n",
                          "BEGIN PHDModelData\n% x(complex)\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n% y(real)\n1\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n% y(real)\n1 2\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n1\n% y(real)\n2\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n# z(real) = 1\n1\nEND\n",
                          "BEGIN PHDModelData\n1\nEND\n",
                          "BEGIN PHDModelData\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n1\n",
                          "VAR x(real) = 1\n",
                          "VAR x(real) 1\n",
                          "END\n",
                          "BEGIN PHDModelData extra\n",
                          "BEGIN PHDModelData\n% x(real)\n1\nEND extra\n",
                          "BEGIN PHDModelData\n% x(string)\n\"unfinished\nEND\n",
                          "BEGIN PHDModelData\n% x(string)\n\"a\"b\nEND\n",
                          "BEGIN PHDModelData\n% x(string)\n\"a\\b\"\nEND\n",
                          "BEGIN PHDModelData\n% x(real)\n1 ! inline unsupported\nEND\n"}) {
        reject(s);
    }
    reject(simple + "VAR next(real) = 2\n");
    reject(std::string("BEGIN PHDModelData\n% x(string)\na") + '\0' + "b\nEND\n");
    reject(attributes + simple);
    reject(attributes + attributes);
    reject("BEGIN XParamData\n% x(real)\n1\nEND\n");
    for (const auto &header :
         {"% Version(real)\n3", "% Version(int)\n2", "% x(real)\n2", "% Version(real)\n2\n2"}) {
        reject(std::string("BEGIN XParamAttributes\n") + header + "\nEND\n");
    }
    try {
        (void)parse("BEGIN PHDModelData\n% x(real)\n  nope\nEND\n");
        throw std::runtime_error("invalid numeric accepted");
    } catch (const XParameterParseError &e) {
        require(e.location().line == 3 && e.location().column == 3,
                "precise numeric error location");
    }
}

void limits() {
    XParameterLimits l;
    l.max_bytes = simple.size();
    (void)parse(simple, l);
    --l.max_bytes;
    reject(simple, l);
    l = {};
    l.max_line_bytes = 18;
    (void)parse(simple, l);
    l.max_line_bytes = 17;
    reject(simple, l);
    l = {};
    l.max_lines = 4;
    (void)parse(simple, l);
    l.max_lines = 3;
    reject(simple, l);
    l = {};
    l.max_blocks = 1;
    (void)parse(simple, l);
    reject(simple + simple, l);
    l = {};
    l.max_columns_per_block = 1;
    (void)parse(simple, l);
    reject("BEGIN PHDModelData\n% x(real) y(int)\n1 2\nEND\n", l);
    l = {};
    l.max_header_groups_per_block = 1;
    (void)parse(simple, l);
    reject("BEGIN PHDModelData\n% x(real)\n% y(int)\n1\n2\nEND\n", l);
    l = {};
    l.max_rows_per_block = 1;
    (void)parse(simple, l);
    reject("BEGIN PHDModelData\n% x(real)\n1\n2\nEND\n", l);
    l = {};
    l.max_total_rows = 1;
    reject(simple + simple, l);
    const std::string declared =
        "VAR v(real) = 1\nBEGIN PHDModelData\n# c(real) = 2\n% z(complex)\n3 4\nEND\n";
    l = {};
    l.max_total_values = 3;
    (void)parse(declared, l);
    l.max_total_values = 2;
    reject(declared, l);
    l = {};
    l.max_declarations_per_block = 3;
    (void)parse(declared, l);
    l.max_declarations_per_block = 2;
    reject(declared, l);
    l = {};
    l.max_total_tokens = 4;
    (void)parse(declared, l);
    l.max_total_tokens = 3;
    reject(declared, l);
    l = {};
    l.max_total_declarations = 1;
    (void)parse(simple, l);
    reject(simple + simple, l);
    l = {};
    l.max_total_text_bytes = 20;
    (void)parse(simple, l);
    l.max_total_text_bytes = 19;
    reject(simple, l);
    using Member = std::size_t XParameterLimits::*;
    const std::array<Member, 13> members{{&XParameterLimits::max_total_text_bytes,
                                          &XParameterLimits::max_total_tokens,
                                          &XParameterLimits::max_total_declarations,
                                          &XParameterLimits::max_bytes,
                                          &XParameterLimits::max_line_bytes,
                                          &XParameterLimits::max_lines,
                                          &XParameterLimits::max_blocks,
                                          &XParameterLimits::max_columns_per_block,
                                          &XParameterLimits::max_header_groups_per_block,
                                          &XParameterLimits::max_rows_per_block,
                                          &XParameterLimits::max_total_rows,
                                          &XParameterLimits::max_total_values,
                                          &XParameterLimits::max_declarations_per_block}};
    for (auto member : members) {
        l = {};
        l.*member = 0;
        reject(simple, l);
    }
}

void ownership_and_streams() {
    auto d = [] {
        std::string s = simple;
        std::istringstream in(s);
        return read_xparameter_data(in);
    }();
    auto copy = d;
    auto moved = std::move(copy);
    require(std::get<double>(at(moved).value) == 1. && at(d).raw_tokens[0] == "1",
            "document owns values and raw tokens");
    std::istringstream bad(simple);
    bad.setstate(std::ios::badbit);
    try {
        (void)read_xparameter_data(bad);
        throw std::runtime_error("bad stream accepted");
    } catch (const XParameterParseError &) {
        ++rejected;
    }
    std::istringstream fail(simple);
    fail.setstate(std::ios::failbit);
    try {
        (void)read_xparameter_data(fail);
        throw std::runtime_error("failed stream accepted");
    } catch (const XParameterParseError &) {
        ++rejected;
    }
    std::istringstream throwing(simple);
    throwing.exceptions(std::ios::badbit | std::ios::failbit);
    require(read_xparameter_data(throwing).blocks().size() == 1, "exception-enabled normal EOF");

    struct BrokenBuffer : std::streambuf {
        int_type underflow() override {
            throw std::ios_base::failure("synthetic I/O fault");
        }
    } buffer;

    std::istream broken(&buffer);
    broken.exceptions(std::ios::badbit);
    try {
        (void)read_xparameter_data(broken);
        throw std::runtime_error("throwing stream accepted");
    } catch (const XParameterParseError &) {
        ++rejected;
    }

    struct RuntimeBuffer : std::streambuf {
        int_type underflow() override {
            throw std::runtime_error("non-ios I/O fault");
        }
    } runtime_buffer;

    std::istream runtime_broken(&runtime_buffer);
    runtime_broken.exceptions(std::ios::badbit);
    try {
        (void)read_xparameter_data(runtime_broken);
        throw std::runtime_error("non-ios throwing stream accepted");
    } catch (const XParameterParseError &) {
        ++rejected;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("rfmodel-xparameter-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mdf");

    struct Remove {
        std::filesystem::path path;

        ~Remove() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{path};

    {
        std::ofstream out(path, std::ios::binary);
        out << attributes << "BEGIN XParamData\n% x(real)\n1\nEND\n";
        require(bool(out), "write path fixture");
    }
    const auto from_path = read_xparameter_data(path);
    require(from_path.blocks().size() == 2 && from_path.version() == XParameterVersion::V2_0,
            "filesystem path overload identifies v2 content despite mdf extension");
    std::filesystem::remove(path);
    try {
        (void)read_xparameter_data(path);
        throw std::runtime_error("missing path accepted");
    } catch (const XParameterParseError &) {
        ++rejected;
    }
}

void evidence_samples() {
    // Text-to-value regressions only, from validation/systemvue-xparameter-format-evidence-20261015.json.
    // These synthetic five-field snippets are NOT full vendor files, simulations,
    // normalization or model compatibility tests. Two legacy filenames share one SHA256.
    struct Sample {
        const char *name;
        const char *tokens;
        std::complex<double> expected;
    };

    const std::array<std::array<Sample, 5>, 3> samples{
        {{{{"FB_2_1", "-0.0751489568 0.555220186", {-0.0751489568, 0.555220186}},
           {"FB_2_2", "0.0333027914 0.0106572701", {0.0333027914, 0.0106572701}},
           {"FB_2_3", "-0.00411803186 0.0149974174", {-0.00411803186, 0.0149974174}},
           {"S_2_1_1_1", "-0.563206932 4.18902719", {-0.563206932, 4.18902719}},
           {"T_2_1_1_1", "0 0", {0, 0}}}},
         {{{"FB_2_1", "0.00642144 0.00765277", {0.00642144, 0.00765277}},
           {"FB_2_2", "0 0", {0, 0}},
           {"FB_2_3", "-2.14866e-06 -2.56068e-06", {-2.14866e-6, -2.56068e-6}},
           {"FB_2_4", "0 0", {0, 0}},
           {"FB_2_5", "-1.20739e-09 -1.43891e-09", {-1.20739e-9, -1.43891e-9}}}},
         {{{"FI_3", "0.0133849", {0.0133849, 0}},
           {"FB_2_0_1", "0.000932938 0.00631275", {0.000932938, 0.00631275}},
           {"FB_2_0_2", "5.54274e-06 -2.05094e-05", {5.54274e-6, -2.05094e-5}},
           {"FB_2_0_3", "6.20086e-07 -4.29281e-07", {6.20086e-7, -4.29281e-7}},
           {"FB_2_1_m2", "-2.06795e-07 9.04205e-07", {-2.06795e-7, 9.04205e-7}}}}}};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::string s = i == 0 ? "BEGIN PHDModelData\n%" : attributes + "BEGIN XParamData\n%";
        for (const auto &p : samples[i]) {
            s += std::string(" ") + p.name +
                 (std::string(p.name) == "FI_3" ? "(real)" : "(complex)");
        }
        s += '\n';
        for (const auto &p : samples[i]) {
            s += std::string(p.tokens) + ' ';
        }
        s += "\nEND\n";
        const auto d = parse(s);
        const auto block = i == 0 ? 0u : 1u;
        for (std::size_t c = 0; c < 5; ++c) {
            const auto &v = at(d, block, 0, 0, c);
            const auto actual = std::holds_alternative<double>(v.value)
                                    ? std::complex<double>(std::get<double>(v.value), 0)
                                    : std::get<std::complex<double>>(v.value);
            require(actual == samples[i][c].expected, "vendor evidence sample text-to-value");
            require(d.blocks()[block].headers[0].fields[c].name == samples[i][c].name,
                    "evidence column name");
        }
    }
}
} // namespace

int main() {
    try {
        structural();
        numeric();
        malformed();
        limits();
        ownership_and_streams();
        evidence_samples();
        std::cout << "X-parameter typed reader: synthetic fixtures, 15 evidence values, "
                  << rejected << " rejected inputs passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
