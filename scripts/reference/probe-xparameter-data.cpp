// Read-only shape/sample probe; no simulation or vendor compatibility assertion.
#ifdef RFMODEL_XPARAMETER_INSTALLED_HEADER
#include RFMODEL_XPARAMETER_INSTALLED_HEADER
#else
#include <rfmodel/io/xparameter_data.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr std::size_t max_json_bytes = 256 * 1024;
constexpr std::size_t max_summary_blocks = 4096;
constexpr std::size_t max_summary_records = 4096;
constexpr std::size_t max_summary_names = 4096;
constexpr std::size_t max_name_bytes = 256;
constexpr std::size_t max_string_bytes = 16 * 1024;

// Messages are fixed, bounded diagnostics: no vendor values or source text.
class SummaryError : public std::runtime_error {
public:
    explicit SummaryError(const char *message) : std::runtime_error(message) {
    }
};

[[noreturn]] void resource_limit() {
    throw SummaryError("resource_limit: JSON summary limit exceeded");
}

void validate_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) {
            continue;
        }
        std::size_t count = 0;
        std::uint32_t point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            point = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            point = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            point = first & 0x07;
            minimum = 0x10000;
        } else {
            throw SummaryError("encoding_error: invalid UTF-8 string");
        }
        if (count > text.size() - i) {
            throw SummaryError("encoding_error: invalid UTF-8 string");
        }
        for (std::size_t j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) {
                throw SummaryError("encoding_error: invalid UTF-8 string");
            }
            point = (point << 6) | (next & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
            throw SummaryError("encoding_error: invalid UTF-8 string");
        }
    }
}

void validate_name(const std::string &name) {
    if (name.size() > max_name_bytes) {
        resource_limit();
    }
    validate_utf8(name);
}

void validate_string(const rfmodel::XParameterValue &value) {
    if (const auto *text = std::get_if<std::string>(&value.value)) {
        if (text->size() > max_string_bytes) {
            resource_limit();
        }
        validate_utf8(*text);
    }
}

const char *type_name(rfmodel::XParameterType type) {
    switch (type) {
    case rfmodel::XParameterType::Integer:
        return "integer";
    case rfmodel::XParameterType::Real:
        return "real";
    case rfmodel::XParameterType::Complex:
        return "complex";
    case rfmodel::XParameterType::String:
        return "string";
    }
    throw SummaryError("summary_error: unknown field type");
}

class Json {
public:
    Json() {
        text_.reserve(max_json_bytes);
    }

    void append(std::string_view value) {
        if (value.size() > max_json_bytes - text_.size()) {
            resource_limit();
        }
        text_.append(value.data(), value.size());
    }

    void append(char value) {
        if (text_.size() == max_json_bytes) {
            resource_limit();
        }
        text_ += value;
    }

    void string(std::string_view value) {
        if (value.size() > max_string_bytes) {
            resource_limit();
        }
        validate_utf8(value);
        append('"');
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') {
                append('\\');
                append(static_cast<char>(c));
            } else if (c < 0x20) {
                append("\\u00");
                append(hex[c >> 4]);
                append(hex[c & 15]);
            } else {
                append(static_cast<char>(c));
            }
        }
        append('"');
    }

    void size(std::size_t value) {
        append(std::to_string(value));
    }

    void real(double value) {
        if (!std::isfinite(value)) {
            throw SummaryError("summary_error: nonfinite number");
        }
        std::ostringstream number;
        number.imbue(std::locale::classic());
        // showpoint retains the JSON real/integer distinction and negative zero.
        number << std::showpoint << std::setprecision(std::numeric_limits<double>::max_digits10)
               << value;
        append(number.str());
    }

    void record(const rfmodel::XParameterField &field, const rfmodel::XParameterValue &value) {
        if (records_ == max_summary_records) {
            resource_limit();
        }
        ++records_;
        append("{\"name\":");
        string(field.name);
        append(",\"type\":");
        string(type_name(field.type));
        append(",\"value\":");
        switch (field.type) {
        case rfmodel::XParameterType::Integer:
            append(std::to_string(std::get<std::int64_t>(value.value)));
            break;
        case rfmodel::XParameterType::Real:
            real(std::get<double>(value.value));
            break;
        case rfmodel::XParameterType::Complex: {
            const auto number = std::get<std::complex<double>>(value.value);
            append('[');
            real(number.real());
            append(',');
            real(number.imag());
            append(']');
            break;
        }
        case rfmodel::XParameterType::String:
            string(std::get<std::string>(value.value));
            break;
        }
        append('}');
    }

    void named_values(const std::vector<rfmodel::XParameterNamedValue> &values) {
        append('[');
        bool first = true;
        for (const auto &value : values) {
            comma(first);
            record(value.field, value.value);
        }
        append(']');
    }

    void comma(bool &first) {
        if (!first) {
            append(',');
        }
        first = false;
    }

    const std::string &text() const {
        return text_;
    }

private:
    std::string text_;
    std::size_t records_{};
};

bool coefficient_block(const rfmodel::XParameterBlock &block) {
    return block.name == "PHDModelData" || block.name == "XParamData";
}

bool sampled_name(const std::string &name) {
    return name.rfind("FI_", 0) == 0 || name.rfind("FB_2_", 0) == 0 ||
           name.rfind("S_2_", 0) == 0 || name.rfind("T_2_", 0) == 0;
}

std::size_t column_count(const rfmodel::XParameterBlock &block) {
    std::size_t count = 0;
    for (const auto &group : block.headers) {
        count += group.fields.size();
    }
    return count;
}

struct Range {
    double minimum, maximum, first;
};

std::string json_summary(const rfmodel::XParameterDocument &doc) {
    if (doc.blocks().size() > max_summary_blocks) {
        resource_limit();
    }
    std::map<std::string, std::size_t> block_counts, families;
    std::map<std::string, Range> ranges;
    std::map<std::string, std::vector<const rfmodel::XParameterBlock *>> metadata;
    std::vector<const rfmodel::XParameterBlock *> coefficients;
    for (const auto &block : doc.blocks()) {
        validate_name(block.name);
        ++block_counts[block.name];
        for (const auto *values : {&block.variables, &block.constants}) {
            for (const auto &value : *values) {
                validate_name(value.field.name);
                validate_string(value.value);
            }
        }
        for (const auto &group : block.headers) {
            for (const auto &field : group.fields) {
                validate_name(field.name);
            }
        }
        // Reject invalid UTF-8 even when the value is outside the small output sample.
        for (const auto &row : block.rows) {
            for (const auto &group : row.groups) {
                for (const auto &value : group.values) {
                    validate_string(value);
                }
            }
        }
        if (!coefficient_block(block)) {
            metadata[block.name].push_back(&block);
            continue;
        }
        const bool first_coefficient = coefficients.empty();
        coefficients.push_back(&block);
        for (std::size_t g = 0; g < block.headers.size(); ++g) {
            for (std::size_t c = 0; c < block.headers[g].fields.size(); ++c) {
                const auto &field = block.headers[g].fields[c];
                if (first_coefficient) {
                    ++families[field.name.substr(0, field.name.find('_'))];
                    if (families.size() > max_summary_names) {
                        resource_limit();
                    }
                }
                if (field.name.rfind("AN_", 0) == 0 &&
                    field.type == rfmodel::XParameterType::Real) {
                    for (const auto &row : block.rows) {
                        const auto x = std::get<double>(row.groups[g].values[c].value);
                        auto inserted = ranges.emplace(field.name, Range{x, x, x});
                        if (ranges.size() > max_summary_names) {
                            resource_limit();
                        }
                        auto &range = inserted.first->second;
                        range.minimum = std::min(range.minimum, x);
                        range.maximum = std::max(range.maximum, x);
                    }
                }
            }
        }
    }
    if (coefficients.empty()) {
        throw SummaryError("summary_error: no coefficient blocks");
    }

    Json json;
    json.append("{\"schema_version\":1,\"reader_version\":");
    json.string(doc.version() == rfmodel::XParameterVersion::Legacy ? "legacy" : "2.0");
    json.append(",\"simulation_executed\":false,\"numerical_model_compatibility\":false,"
                "\"all_data_group_shapes_checked\":true,\"block_counts\":{");
    bool first = true;
    for (const auto &entry : block_counts) {
        json.comma(first);
        json.string(entry.first);
        json.append(':');
        json.size(entry.second);
    }
    json.append("},\"coefficient_rows_per_block\":[");
    first = true;
    for (const auto *block : coefficients) {
        json.comma(first);
        json.size(block->rows.size());
    }
    json.append("],\"coefficient_columns_per_row\":[");
    first = true;
    for (const auto *block : coefficients) {
        json.comma(first);
        json.size(column_count(*block));
    }
    json.append("],\"first_block_column_families\":{");
    first = true;
    for (const auto &entry : families) {
        json.comma(first);
        json.string(entry.first);
        json.append(':');
        json.size(entry.second);
    }
    json.append("},\"AN_raw_ranges\":{");
    first = true;
    for (const auto &entry : ranges) {
        json.comma(first);
        json.string(entry.first);
        json.append(":{\"min\":");
        json.real(entry.second.minimum);
        json.append(",\"max\":");
        json.real(entry.second.maximum);
        json.append(",\"first\":");
        json.real(entry.second.first);
        json.append('}');
    }
    json.append("},\"first_row_small_sample\":[");
    std::size_t samples = 0;
    const auto &sample_block = *coefficients.front();
    first = true;
    for (std::size_t g = 0; g < sample_block.headers.size(); ++g) {
        for (std::size_t c = 0; c < sample_block.headers[g].fields.size(); ++c) {
            const auto &field = sample_block.headers[g].fields[c];
            if (samples < 5 && sampled_name(field.name)) {
                json.comma(first);
                json.record(field, sample_block.rows.front().groups[g].values[c]);
                ++samples;
            }
        }
    }
    json.append("],\"metadata_blocks\":{");
    first = true;
    for (const auto &entry : metadata) {
        json.comma(first);
        json.string(entry.first);
        json.append(":[");
        bool first_row = true;
        for (const auto *block : entry.second) {
            for (const auto &row : block->rows) {
                json.comma(first_row);
                json.append('[');
                bool first_value = true;
                for (std::size_t g = 0; g < block->headers.size(); ++g) {
                    for (std::size_t c = 0; c < block->headers[g].fields.size(); ++c) {
                        json.comma(first_value);
                        json.record(block->headers[g].fields[c], row.groups[g].values[c]);
                    }
                }
                json.append(']');
            }
        }
        json.append(']');
    }
    json.append("},\"coefficient_block_outer_variables\":[");
    first = true;
    for (const auto *block : coefficients) {
        json.comma(first);
        json.named_values(block->variables);
    }
    json.append("],\"legacy_constants\":[");
    first = true;
    for (const auto *block : coefficients) {
        if (!block->constants.empty()) {
            json.comma(first);
            json.named_values(block->constants);
        }
    }
    json.append("]}\n");
    return json.text();
}

void print_value(const rfmodel::XParameterValue &v) {
    for (std::size_t i = 0; i < v.raw_tokens.size(); ++i) {
        if (i) {
            std::cout << ' ';
        }
        std::cout << v.raw_tokens[i];
    }
}

void text_summary(const rfmodel::XParameterDocument &doc) {
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    std::cout << "Raw typed text only; numerical model compatibility is NOT established.\n"
              << "version="
              << (doc.version() == rfmodel::XParameterVersion::Legacy ? "legacy" : "2.0") << '\n';
    std::map<std::string, std::pair<double, double>> ranges;
    for (std::size_t bi = 0; bi < doc.blocks().size(); ++bi) {
        const auto &b = doc.blocks()[bi];
        std::cout << "block=" << bi << " name=" << b.name << " rows=" << b.rows.size()
                  << " columns=" << column_count(b) << " groups=" << b.headers.size() << '\n';
        for (const auto &v : b.variables) {
            std::cout << "VAR " << v.field.name << " = ";
            print_value(v.value);
            std::cout << '\n';
        }
        for (const auto &v : b.constants) {
            std::cout << "# " << v.field.name << " = ";
            print_value(v.value);
            std::cout << '\n';
        }
        std::size_t samples = 0;
        for (std::size_t g = 0; g < b.headers.size(); ++g) {
            for (std::size_t c = 0; c < b.headers[g].fields.size(); ++c) {
                const auto &f = b.headers[g].fields[c];
                if (f.name.rfind("AN_", 0) == 0 && f.type == rfmodel::XParameterType::Real) {
                    for (const auto &row : b.rows) {
                        const auto x = std::get<double>(row.groups[g].values[c].value);
                        const auto inserted = ranges.emplace(f.name, std::make_pair(x, x));
                        auto &range = inserted.first->second;
                        range.first = std::min(range.first, x);
                        range.second = std::max(range.second, x);
                    }
                }
                if (samples < 5 && sampled_name(f.name)) {
                    std::cout << "first-row " << f.name << " = ";
                    print_value(b.rows.front().groups[g].values[c]);
                    std::cout << '\n';
                    ++samples;
                }
            }
        }
    }
    for (const auto &r : ranges) {
        std::cout << "AN-range " << r.first << " min=" << r.second.first
                  << " max=" << r.second.second << '\n';
    }
}

void binary_standard_streams(bool input, bool output) {
#ifdef _WIN32
    // Hash-bound stdin must not translate CRLF or treat Ctrl-Z as EOF on Windows.
    if ((input && _setmode(_fileno(stdin), _O_BINARY) == -1) ||
        (output && _setmode(_fileno(stdout), _O_BINARY) == -1)) {
        throw SummaryError("input_output_error: cannot select binary stream mode");
    }
#else
    (void)input;
    (void)output;
#endif
}
} // namespace

int main(int argc, char **argv) {
    bool json = false, use_stdin = false, options = true;
    const char *path = nullptr;
    bool invalid = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (options && argument == "--json" && !json) {
            json = true;
        } else if (options && argument == "--stdin" && !use_stdin) {
            use_stdin = true;
        } else if (options && argument == "--") {
            options = false;
        } else if (path || (options && argument.rfind("--", 0) == 0)) {
            invalid = true;
        } else {
            path = argv[i];
        }
    }
    if (invalid || (use_stdin == (path != nullptr))) {
        std::cerr << "usage: probe-xparameter-data [--json] (<local-file> | --stdin)\n";
        return 2;
    }
    try {
        binary_standard_streams(use_stdin, json);
        const rfmodel::XParameterLimits limits; // Includes the 64 MiB input limit.
        const auto doc =
            use_stdin ? rfmodel::read_xparameter_data(std::cin, limits)
                      : rfmodel::read_xparameter_data(std::filesystem::path(path), limits);
        if (json) {
            // Parsing, validation and bounded serialization all finish before any stdout.
            const auto output = json_summary(doc);
            std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
        } else {
            text_summary(doc);
        }
        std::cout.flush();
        if (!std::cout) {
            throw SummaryError("input_output_error: cannot write output");
        }
    } catch (const rfmodel::XParameterParseError &e) {
        // Native diagnostics may contain field names or paths; do not export those.
        const bool limit =
            std::string_view(e.what()).find(" limit exceeded") != std::string_view::npos;
        std::cerr << (limit ? "resource_limit" : "parse_error") << ": line " << e.location().line
                  << ", column " << e.location().column << '\n';
        return 1;
    } catch (const SummaryError &e) {
        std::cerr << e.what() << '\n';
        return 1;
    } catch (const std::bad_alloc &) {
        std::cerr << "resource_limit: allocation failed\n";
        return 1;
    } catch (const std::exception &) {
        std::cerr << "probe_error: read or summary failed\n";
        return 1;
    }
    return 0;
}
