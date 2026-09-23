#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <locale>
#include <regex>
#include <sstream>

namespace rfmodel {
enum class TouchstoneFormat {
    RI,
    MA,
    DB
};

struct TouchstoneData {
    std::size_t ports{};
    double frequency_scale{1.0};
    TouchstoneFormat format{TouchstoneFormat::RI};
    std::vector<double> frequencies_hz;
    std::vector<SMatrix> matrices;
    double reference_impedance_ohms{50.0};
};

namespace touchstone_detail {
inline std::string upper(std::string s) {
    for (auto &c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

inline double number(const std::string &s) {
    std::istringstream input(s);
    input.imbue(std::locale::classic());
    double value;
    if (!(input >> value) || input.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(value)) {
        throw std::runtime_error("invalid finite Touchstone number: " + s);
    }
    return value;
}
} // namespace touchstone_detail

// Legacy full-matrix S data. Version 2 keywords are explicitly rejected.
inline TouchstoneData read_touchstone(const std::string &path) {
    std::smatch match;
    if (!std::regex_search(path, match, std::regex("\\.s([1-9][0-9]*)p$", std::regex::icase))) {
        throw std::runtime_error("expected .sNp extension");
    }
    TouchstoneData d;
    // Explicit allocation limit for this reader, not a Touchstone format limit.
    const double ports = touchstone_detail::number(match[1].str());
    if (ports > 1024) {
        throw std::runtime_error("reader supports at most 1024 ports");
    }
    d.ports = static_cast<std::size_t>(ports);
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open Touchstone file: " + path);
    }
    bool header = false;
    std::vector<double> record;
    const auto size = 1 + 2 * d.ports * d.ports;
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('!'));
        std::istringstream words(line);
        std::string token;
        if (!(words >> token)) {
            continue;
        }
        if (token[0] == '[') {
            throw std::runtime_error("Touchstone 2 keywords not yet supported");
        }
        if (token == "#") {
            if (header) {
                throw std::runtime_error("duplicate option line");
            }
            std::vector<std::string> options;
            while (words >> token) {
                options.push_back(touchstone_detail::upper(token));
            }
            const std::vector<std::string> defaults{"GHZ", "S", "MA", "R", "50"};
            if (options.size() > defaults.size()) {
                throw std::runtime_error("extra option fields");
            }
            while (options.size() < defaults.size()) {
                options.push_back(defaults[options.size()]);
            }
            const auto &unit = options[0];
            if (unit == "HZ") {
                d.frequency_scale = 1;
            } else if (unit == "KHZ") {
                d.frequency_scale = 1e3;
            } else if (unit == "MHZ") {
                d.frequency_scale = 1e6;
            } else if (unit == "GHZ") {
                d.frequency_scale = 1e9;
            } else {
                throw std::runtime_error("unsupported frequency unit");
            }
            if (options[1] != "S") {
                throw std::runtime_error("only S parameters supported");
            }
            if (options[2] == "RI") {
                d.format = TouchstoneFormat::RI;
            } else if (options[2] == "MA") {
                d.format = TouchstoneFormat::MA;
            } else if (options[2] == "DB") {
                d.format = TouchstoneFormat::DB;
            } else {
                throw std::runtime_error("unsupported data format");
            }
            if (options[3] != "R") {
                throw std::runtime_error("expected reference resistance R");
            }
            d.reference_impedance_ohms = touchstone_detail::number(options[4]);
            if (d.reference_impedance_ohms <= 0) {
                throw std::runtime_error("reference must be positive");
            }
            header = true;
            continue;
        }
        if (!header) {
            throw std::runtime_error("missing option line");
        }
        do {
            record.push_back(touchstone_detail::number(token));
        } while (words >> token);
        if (record.size() > size) {
            throw std::runtime_error("extra data in frequency record");
        }
        if (record.size() < size) {
            continue;
        }
        const double f = record[0] * d.frequency_scale;
        if (!std::isfinite(f) || f < 0 ||
            (!d.frequencies_hz.empty() && f <= d.frequencies_hz.back())) {
            throw std::runtime_error(
                "frequencies must be finite, nonnegative and strictly increasing");
        }
        SMatrix m{d.ports, std::vector<Complex>(d.ports * d.ports)};
        for (std::size_t i = 0; i < m.values.size(); ++i) {
            const double a = record[1 + 2 * i], b = record[2 + 2 * i];
            Complex value;
            if (d.format == TouchstoneFormat::RI) {
                value = {a, b};
            } else {
                if (d.format == TouchstoneFormat::MA && a < 0) {
                    throw std::runtime_error("negative magnitude");
                }
                value = std::polar(d.format == TouchstoneFormat::DB ? std::pow(10.0, a / 20.0) : a,
                                   b * (3.14159265358979323846 / 180.0));
            }
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                throw std::runtime_error("wave overflow");
            }
            // Legacy two-port order is S11,S21,S12,S22; others are row-major.
            const auto index = d.ports == 2 ? (i % 2) * 2 + i / 2 : i;
            m.values[index] = value;
        }
        d.frequencies_hz.push_back(f);
        d.matrices.push_back(std::move(m));
        record.clear();
    }
    if (!in.eof()) {
        throw std::runtime_error("Touchstone read error");
    }
    if (!header || d.matrices.empty() || !record.empty()) {
        throw std::runtime_error("empty or incomplete Touchstone data");
    }
    return d;
}
} // namespace rfmodel
