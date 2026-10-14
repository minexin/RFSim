#pragma once
#include "rfmodel/core/device_model.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <regex>
#include <set>
#include <sstream>

namespace rfmodel {
enum class TouchstoneFormat {
    RI,
    MA,
    DB
};
enum class TouchstoneVersion {
    Legacy,
    V2_0
};
enum class TouchstoneMatrixFormat {
    Full,
    Lower,
    Upper
};
enum class TouchstoneTwoPortOrder {
    Order21_12,
    Order12_21
};

struct TouchstoneNoiseSample {
    double frequency_hz{};
    double minimum_noise_figure_db{};
    Complex optimum_source_reflection{};
    double noise_resistance_ohms{};
};

struct TouchstoneData {
    std::size_t ports{};
    double frequency_scale{1.0};
    TouchstoneFormat format{TouchstoneFormat::RI};
    std::vector<double> frequencies_hz;
    std::vector<SMatrix> matrices;
    // Option-line R. Also the reference for GammaOpt, even with [Reference].
    double reference_impedance_ohms{50.0};
    std::vector<TouchstoneNoiseSample> noise_samples;
    TouchstoneVersion version{TouchstoneVersion::Legacy};
    TouchstoneMatrixFormat matrix_format{TouchstoneMatrixFormat::Full};
    TouchstoneTwoPortOrder two_port_order{TouchstoneTwoPortOrder::Order21_12};
    // Actual S references, in port order. Empty preserves legacy hand-built data.
    std::vector<double> reference_impedances_ohms;
};

inline std::vector<Complex> effective_reference_impedances(const TouchstoneData &data) {
    if (!std::isfinite(data.reference_impedance_ohms) || data.reference_impedance_ohms <= 0.) {
        throw std::invalid_argument("invalid option-line reference resistance");
    }
    if (!data.ports || data.ports > 1024 ||
        (!data.reference_impedances_ohms.empty() &&
         data.reference_impedances_ohms.size() != data.ports)) {
        throw std::invalid_argument("one Touchstone reference resistance is required per port");
    }
    std::vector<Complex> result(data.ports, data.reference_impedance_ohms);
    for (std::size_t i = 0; i < data.reference_impedances_ohms.size(); ++i) {
        const auto value = data.reference_impedances_ohms[i];
        if (!std::isfinite(value) || value <= 0.) {
            throw std::invalid_argument(
                "Touchstone references must be finite positive resistances");
        }
        result[i] = value;
    }
    return result;
}

// For interfaces that can represent only one common real reference.
inline double common_reference_impedance(const TouchstoneData &data) {
    const auto refs = effective_reference_impedances(data);
    for (const auto ref : refs) {
        if (ref != refs.front()) {
            throw std::invalid_argument("this interface requires a common Touchstone reference; "
                                        "use the native C++ per-port API for unequal references");
        }
    }
    return refs.front().real();
}

namespace touchstone_detail {
inline std::string upper(std::string s) {
    for (auto &c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{}
                                      : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

inline double number(const std::string &s) {
    // Validate the complete decimal token before interpreting stream range errors.
    // In particular, a nonzero significand that rounds to zero is underflow, not zero.
    std::size_t position = 0;
    if (position < s.size() && (s[position] == '+' || s[position] == '-')) {
        ++position;
    }
    bool nonzero = false;
    const auto digits = [&] {
        const auto start = position;
        while (position < s.size() && s[position] >= '0' && s[position] <= '9') {
            nonzero = nonzero || s[position] != '0';
            ++position;
        }
        return position != start;
    };
    bool significand = digits();
    if (position < s.size() && s[position] == '.') {
        ++position;
        significand = digits() || significand;
    }
    const bool nonzero_significand = nonzero;
    bool exponent = true;
    if (position < s.size() && (s[position] == 'e' || s[position] == 'E')) {
        ++position;
        if (position < s.size() && (s[position] == '+' || s[position] == '-')) {
            ++position;
        }
        exponent = digits();
    }
    if (!significand || !exponent || position != s.size()) {
        throw std::runtime_error("invalid finite Touchstone number: " + s);
    }
    std::istringstream input(s);
    input.imbue(std::locale::classic());
    double value = 0.;
    input >> value;
    // libc++ and MSVC may set failbit on strtod's ERANGE even when the
    // conversion produced a representable nonzero subnormal. Include the
    // min-normal rounding boundary, but never forgive overflow or badbit.
    const bool tiny = value != 0. && std::abs(value) <= std::numeric_limits<double>::min();
    if (input.bad() || (input.fail() && !tiny) ||
        input.rdbuf()->sgetc() != std::char_traits<char>::eof() || !std::isfinite(value) ||
        (value == 0. && nonzero_significand)) {
        throw std::runtime_error("invalid finite Touchstone number: " + s);
    }
    return value;
}

inline std::size_t positive_integer(const std::string &s) {
    std::size_t value = 0;
    if (s.empty()) {
        throw std::runtime_error("missing positive Touchstone integer");
    }
    for (const auto c : s) {
        if (c < '0' || c > '9' ||
            value > (std::numeric_limits<std::size_t>::max() - (c - '0')) / 10) {
            throw std::runtime_error("invalid positive Touchstone integer: " + s);
        }
        value = value * 10 + (c - '0');
    }
    if (!value) {
        throw std::runtime_error("Touchstone count must be positive");
    }
    return value;
}

inline std::vector<double> numbers(const std::string &line) {
    std::istringstream input(line);
    std::vector<double> result;
    std::string token;
    while (input >> token) {
        result.push_back(number(token));
    }
    return result;
}

inline void options(TouchstoneData &data, const std::string &line) {
    data.frequency_scale = 1e9;
    data.format = TouchstoneFormat::MA;
    std::istringstream input(line.substr(1));
    std::set<std::string> categories;
    std::string token;
    while (input >> token) {
        token = upper(token);
        std::string category;
        if (token == "HZ" || token == "KHZ" || token == "MHZ" || token == "GHZ") {
            category = "unit";
            data.frequency_scale = token == "HZ"    ? 1.
                                   : token == "KHZ" ? 1e3
                                   : token == "MHZ" ? 1e6
                                                    : 1e9;
        } else if (token == "S") {
            category = "parameter";
        } else if (token == "Y" || token == "Z" || token == "G" || token == "H") {
            throw std::runtime_error("only single-ended S parameters supported");
        } else if (token == "RI" || token == "MA" || token == "DB") {
            category = "format";
            data.format = token == "RI"   ? TouchstoneFormat::RI
                          : token == "MA" ? TouchstoneFormat::MA
                                          : TouchstoneFormat::DB;
        } else if (token == "R") {
            category = "reference";
            if (!(input >> token)) {
                throw std::runtime_error("missing option-line reference resistance");
            }
            data.reference_impedance_ohms = number(token);
            if (data.reference_impedance_ohms <= 0.) {
                throw std::runtime_error("reference must be positive");
            }
        } else {
            throw std::runtime_error("unsupported Touchstone option: " + token);
        }
        if (!categories.insert(category).second) {
            throw std::runtime_error("duplicate Touchstone option category");
        }
    }
}

inline Complex wave(double a, double b, TouchstoneFormat format) {
    Complex value;
    if (format == TouchstoneFormat::RI) {
        value = {a, b};
    } else {
        if (format == TouchstoneFormat::MA && a < 0.) {
            throw std::runtime_error("negative magnitude");
        }
        value = std::polar(format == TouchstoneFormat::DB ? std::pow(10., a / 20.) : a,
                           std::remainder(b, 360.) * (3.14159265358979323846 / 180.));
    }
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
        throw std::runtime_error("wave overflow");
    }
    return value;
}

inline void
network_record(TouchstoneData &data, std::vector<double> &record, const std::vector<double> &row) {
    const auto pairs = data.matrix_format == TouchstoneMatrixFormat::Full
                           ? data.ports * data.ports
                           : data.ports * (data.ports + 1) / 2;
    const auto size = 1 + 2 * pairs;
    if (row.size() > size - record.size()) {
        throw std::runtime_error("extra data in frequency record");
    }
    record.insert(record.end(), row.begin(), row.end());
    if (record.size() != size) {
        return;
    }
    const double f = record[0] * data.frequency_scale;
    if (!std::isfinite(f) || f < 0. ||
        (!data.frequencies_hz.empty() && f <= data.frequencies_hz.back())) {
        throw std::runtime_error("frequencies must be finite, nonnegative and strictly increasing");
    }
    SMatrix matrix{data.ports, std::vector<Complex>(data.ports * data.ports)};
    std::size_t pair = 0;
    for (std::size_t r = 0; r < data.ports; ++r) {
        for (std::size_t c = 0; c < data.ports; ++c) {
            if ((data.matrix_format == TouchstoneMatrixFormat::Lower && c > r) ||
                (data.matrix_format == TouchstoneMatrixFormat::Upper && c < r)) {
                continue;
            }
            const auto value = wave(record[1 + 2 * pair], record[2 + 2 * pair], data.format);
            ++pair;
            if (data.matrix_format != TouchstoneMatrixFormat::Full) {
                matrix(r, c) = value;
                matrix(c, r) = value; // Complex symmetric, not Hermitian.
            } else if (data.ports == 2 &&
                       data.two_port_order == TouchstoneTwoPortOrder::Order21_12) {
                matrix(c, r) = value;
            } else {
                matrix(r, c) = value;
            }
        }
    }
    data.frequencies_hz.push_back(f);
    data.matrices.push_back(std::move(matrix));
    record.clear();
}

inline void noise_record(TouchstoneData &data, const std::vector<double> &row) {
    if (row.size() != 5 || data.ports != 2 || data.frequencies_hz.empty()) {
        throw std::runtime_error("noise requires a two-port, five-column record on one line");
    }
    const double frequency = row[0] * data.frequency_scale;
    if (!std::isfinite(frequency) || frequency < 0. ||
        (data.noise_samples.empty() ? frequency > data.frequencies_hz.back()
                                    : frequency <= data.noise_samples.back().frequency_hz)) {
        throw std::runtime_error("invalid noise frequency order or first noise frequency");
    }
    if (row[2] < 0. || row[4] < 0.) {
        throw std::runtime_error("negative noise reflection magnitude or resistance");
    }
    const auto gamma = wave(row[2], row[3], TouchstoneFormat::MA);
    const double resistance =
        row[4] * (data.version == TouchstoneVersion::Legacy ? data.reference_impedance_ohms : 1.);
    if (!std::isfinite(resistance)) {
        throw std::runtime_error("noise resistance overflow");
    }
    data.noise_samples.push_back({frequency, row[1], gamma, resistance});
}
} // namespace touchstone_detail

// Bounded single-ended S reader: legacy .sNp and explicit Version 2.0 .ts/.sNp.
// Unsupported extensions (including mixed mode and information blocks) fail explicitly.
inline TouchstoneData read_touchstone(const std::string &path) {
    using namespace touchstone_detail;
    const auto extension = upper(std::filesystem::u8path(path).extension().u8string());
    std::smatch match;
    std::size_t extension_ports = 0;
    if (std::regex_match(extension, match, std::regex("\\.S([1-9][0-9]*)P"))) {
        extension_ports = positive_integer(match[1].str());
        if (extension_ports > 1024) {
            throw std::runtime_error("reader supports at most 1024 ports");
        }
    } else if (extension != ".TS") {
        throw std::runtime_error("expected .sNp or .ts extension");
    }
    std::ifstream input(std::filesystem::u8path(path));
    if (!input) {
        throw std::runtime_error("cannot open Touchstone file: " + path);
    }
    TouchstoneData data;
    data.ports = extension_ports;
    bool first = true, header = false, declared_ports = false;
    bool network = false, noise = false, ended = false, references = false;
    std::size_t network_count = 0, noise_count = 0;
    std::set<std::string> keywords;
    std::vector<double> record;
    std::string line;
    while (std::getline(input, line)) {
        for (const unsigned char c : line) {
            if (c < 32 && c != '\t' && c != '\r') {
                throw std::runtime_error("unsupported control character in Touchstone file");
            }
        }
        line = trim(line.substr(0, line.find('!')));
        if (line.empty()) {
            continue;
        }
        if (ended) {
            throw std::runtime_error("non-comment content after [End]");
        }
        if (first) {
            first = false;
            if (line.front() == '[') {
                const auto closing = line.find(']');
                if (closing == std::string::npos ||
                    upper(line.substr(0, closing + 1)) != "[VERSION]" ||
                    trim(line.substr(closing + 1)) != "2.0") {
                    throw std::runtime_error("only explicit Touchstone [Version] 2.0 is supported");
                }
                data.version = TouchstoneVersion::V2_0;
                continue;
            }
            if (extension == ".TS") {
                throw std::runtime_error(".ts requires explicit Touchstone [Version] 2.0");
            }
        }
        if (line.front() == '#') {
            if (header || network) {
                throw std::runtime_error("duplicate or misplaced option line is unsupported");
            }
            options(data, line);
            header = true;
            continue;
        }
        if (!header) {
            throw std::runtime_error("missing option line");
        }
        if (line.front() == '[') {
            if (data.version != TouchstoneVersion::V2_0) {
                throw std::runtime_error("Touchstone keywords require [Version] 2.0");
            }
            const auto closing = line.find(']');
            if (closing == std::string::npos) {
                throw std::runtime_error("unterminated Touchstone keyword");
            }
            const auto keyword = upper(line.substr(0, closing + 1));
            const auto argument = trim(line.substr(closing + 1));
            if (!keywords.insert(keyword).second) {
                throw std::runtime_error("duplicate Touchstone keyword: " + keyword);
            }
            if (references) {
                if (data.reference_impedances_ohms.size() != data.ports) {
                    throw std::runtime_error("[Reference] requires one resistance per port");
                }
                references = false;
            }
            if (!declared_ports) {
                if (keyword != "[NUMBER OF PORTS]") {
                    throw std::runtime_error("[Number of Ports] must follow the option line");
                }
                data.ports = positive_integer(argument);
                if (data.ports > 1024 || (extension_ports && extension_ports != data.ports)) {
                    throw std::runtime_error(
                        "unsupported port count or .sNp extension/count mismatch");
                }
                declared_ports = true;
            } else if (keyword == "[END]") {
                if (!argument.empty() || !network || !record.empty()) {
                    throw std::runtime_error("invalid [End] or incomplete network record");
                }
                ended = true;
            } else if (keyword == "[NOISE DATA]") {
                if (!argument.empty() || !network || !record.empty() || !noise_count ||
                    data.matrices.size() != network_count) {
                    throw std::runtime_error("invalid [Noise Data] or network count mismatch");
                }
                noise = true;
            } else if (network) {
                throw std::runtime_error("unsupported or misplaced Touchstone keyword: " + keyword);
            } else if (keyword == "[NUMBER OF FREQUENCIES]") {
                network_count = positive_integer(argument);
            } else if (keyword == "[NUMBER OF NOISE FREQUENCIES]") {
                noise_count = positive_integer(argument);
                if (data.ports != 2) {
                    throw std::runtime_error("Touchstone noise data requires two ports");
                }
            } else if (keyword == "[TWO-PORT DATA ORDER]") {
                if (data.ports != 2 || (argument != "21_12" && argument != "12_21")) {
                    throw std::runtime_error("invalid [Two-Port Data Order]");
                }
                data.two_port_order = argument == "21_12" ? TouchstoneTwoPortOrder::Order21_12
                                                          : TouchstoneTwoPortOrder::Order12_21;
            } else if (keyword == "[MATRIX FORMAT]") {
                const auto format = upper(argument);
                if (format != "FULL" && format != "LOWER" && format != "UPPER") {
                    throw std::runtime_error("unsupported [Matrix Format]");
                }
                data.matrix_format = format == "FULL"    ? TouchstoneMatrixFormat::Full
                                     : format == "LOWER" ? TouchstoneMatrixFormat::Lower
                                                         : TouchstoneMatrixFormat::Upper;
            } else if (keyword == "[REFERENCE]") {
                references = true;
                data.reference_impedances_ohms = numbers(argument);
            } else if (keyword == "[NETWORK DATA]") {
                if (!argument.empty() || !network_count ||
                    (data.ports == 2 && !keywords.count("[TWO-PORT DATA ORDER]"))) {
                    throw std::runtime_error(
                        "missing network count/order or invalid [Network Data]");
                }
                network = true;
            } else {
                throw std::runtime_error("unsupported Touchstone keyword: " + keyword);
            }
            continue;
        }
        const auto row = numbers(line);
        if (row.empty()) {
            throw std::runtime_error("empty Touchstone numeric record");
        }
        if (references) {
            if (row.size() >
                data.ports - std::min(data.ports, data.reference_impedances_ohms.size())) {
                throw std::runtime_error("too many [Reference] resistances");
            }
            data.reference_impedances_ohms.insert(
                data.reference_impedances_ohms.end(), row.begin(), row.end());
            continue;
        }
        if (data.version == TouchstoneVersion::V2_0 && !network) {
            throw std::runtime_error("numeric data before [Network Data]");
        }
        if (data.version == TouchstoneVersion::Legacy && record.empty() && data.ports == 2 &&
            !data.frequencies_hz.empty() &&
            row.front() * data.frequency_scale <= data.frequencies_hz.back()) {
            noise = true;
        }
        if (noise) {
            noise_record(data, row);
            if (data.version == TouchstoneVersion::V2_0 &&
                data.noise_samples.size() > noise_count) {
                throw std::runtime_error("noise frequency count exceeded");
            }
        } else {
            network_record(data, record, row);
            if (data.version == TouchstoneVersion::V2_0 && data.matrices.size() > network_count) {
                throw std::runtime_error("network frequency count exceeded");
            }
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Touchstone read error");
    }
    if (!header || data.matrices.empty() || !record.empty()) {
        throw std::runtime_error("empty or incomplete Touchstone data");
    }
    if (data.version == TouchstoneVersion::V2_0 &&
        (!ended || data.matrices.size() != network_count ||
         data.noise_samples.size() != noise_count)) {
        throw std::runtime_error("missing [End] or declared Touchstone frequency count mismatch");
    }
    if (data.reference_impedances_ohms.empty()) {
        data.reference_impedances_ohms.assign(data.ports, data.reference_impedance_ohms);
    }
    // Report malformed file references as reader errors, keeping legacy exception behavior.
    try {
        effective_reference_impedances(data);
    } catch (const std::invalid_argument &error) {
        throw std::runtime_error(error.what());
    }
    return data;
}
} // namespace rfmodel
