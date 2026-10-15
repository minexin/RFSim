#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <locale>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rfmodel {
// Raw typed GMDIF data only: no wave, normalization, or device-model conversion.
enum class XParameterType {
    Integer,
    Real,
    Complex,
    String
};
enum class XParameterVersion {
    Legacy,
    V2_0
};

struct XParameterLocation {
    std::size_t line{1};
    std::size_t column{1};
};

class XParameterParseError : public std::runtime_error {
public:
    XParameterParseError(XParameterLocation location, const std::string &message)
        : std::runtime_error("X-parameter line " + std::to_string(location.line) + ", column " +
                             std::to_string(location.column) + ": " + message),
          location_(location) {
    }

    XParameterLocation location() const noexcept {
        return location_;
    }

private:
    XParameterLocation location_;
};

struct XParameterLimits {
    std::size_t max_bytes{64 * 1024 * 1024};
    std::size_t max_line_bytes{1024 * 1024};
    std::size_t max_lines{1000000};
    std::size_t max_blocks{4096};
    std::size_t max_columns_per_block{16384};
    std::size_t max_header_groups_per_block{16384};
    std::size_t max_rows_per_block{100000};
    std::size_t max_total_rows{1000000};
    std::size_t max_total_text_bytes{128 * 1024 * 1024}; // Conservative parsed-string budget.
    std::size_t max_total_tokens{8000000};
    std::size_t max_total_declarations{1000000};
    std::size_t max_total_values{4000000}; // Includes VAR and # values; complex counts once.
    std::size_t max_declarations_per_block{16384}; // VAR + # + header fields.
};

struct XParameterField {
    std::string name;
    XParameterType type;
    XParameterLocation location;
};

struct XParameterValue {
    std::variant<std::int64_t, double, std::complex<double>, std::string> value;
    std::vector<std::string> raw_tokens; // Quotes retained; complex is two RI tokens.
    XParameterLocation location;
};

struct XParameterNamedValue {
    XParameterField field;
    XParameterValue value;
};

struct XParameterHeaderGroup {
    std::vector<XParameterField> fields;
    XParameterLocation location;
};

struct XParameterDataLine {
    std::vector<XParameterValue> values;
    XParameterLocation location;
};

struct XParameterRow {
    std::vector<XParameterDataLine> groups;
};

struct XParameterBlock {
    std::string name;
    XParameterLocation location;
    std::vector<XParameterNamedValue> variables;
    std::vector<XParameterNamedValue> constants;
    std::vector<XParameterHeaderGroup> headers;
    std::vector<XParameterRow> rows;
};

namespace xparameter_detail {
class Reader;
}

class XParameterDocument {
public:
    const std::vector<XParameterBlock> &blocks() const noexcept {
        return blocks_;
    }

    XParameterVersion version() const noexcept {
        return version_;
    }

private:
    friend class xparameter_detail::Reader;
    XParameterVersion version_{XParameterVersion::Legacy};
    std::vector<XParameterBlock> blocks_;
};

namespace xparameter_detail {
inline bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

inline bool digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '_';
}

[[noreturn]] inline void fail(XParameterLocation p, const std::string &m) {
    throw XParameterParseError(p, m);
}

inline void room(std::size_t used,
                 std::size_t extra,
                 std::size_t limit,
                 XParameterLocation p,
                 const char *what) {
    if (used > limit || extra > limit - used) {
        fail(p, std::string(what) + " limit exceeded");
    }
}

struct TextBudget {
    std::size_t text{}, tokens{}, declarations{};
    const XParameterLimits &limits;

    void charge(std::size_t n, XParameterLocation p) {
        room(text, n, limits.max_total_text_bytes, p, "parsed text bytes");
        text += n;
    }
};

struct Token {
    std::string raw;
    std::string decoded;
    XParameterLocation location;
    bool quoted{};
};

class Line {
public:
    Line(const std::string &text, std::size_t number, TextBudget &budget)
        : text_(text), number_(number), budget_(budget) {
        if (number == 1 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            cursor_ = 3;
        }
    }

    XParameterLocation location() const {
        return {number_, cursor_ + 1};
    }

    void skip() {
        while (cursor_ < text_.size() && space(text_[cursor_])) {
            ++cursor_;
        }
    }

    bool end() {
        skip();
        return cursor_ == text_.size();
    }

    void separator() const {
        if (cursor_ < text_.size() && !space(text_[cursor_])) {
            fail(location(), "missing declaration separator");
        }
    }

    char peek() {
        skip();
        return cursor_ == text_.size() ? '\0' : text_[cursor_];
    }

    bool keyword(const std::string &word) {
        skip();
        if (text_.compare(cursor_, word.size(), word) != 0) {
            return false;
        }
        const auto next = cursor_ + word.size();
        if (next < text_.size() && !space(text_[next])) {
            return false;
        }
        cursor_ = next;
        return true;
    }

    void symbol(char c) {
        skip();
        if (cursor_ == text_.size() || text_[cursor_] != c) {
            fail(location(), std::string("expected '") + c + "'");
        }
        ++cursor_;
    }

    std::string name() {
        skip();
        const auto begin = cursor_;
        while (cursor_ < text_.size() && name_char(text_[cursor_])) {
            ++cursor_;
        }
        if (begin == cursor_) {
            fail(location(), "expected name (ASCII letters, digits, underscore)");
        }
        budget_.charge(cursor_ - begin, {number_, begin + 1});
        return text_.substr(begin, cursor_ - begin);
    }

    XParameterField field() {
        skip();
        const auto p = location();
        auto n = name();
        symbol('(');
        auto t = name();
        symbol(')');
        XParameterType type;
        if (t == "int") {
            type = XParameterType::Integer;
        } else if (t == "real") {
            type = XParameterType::Real;
        } else if (t == "complex") {
            type = XParameterType::Complex;
        } else if (t == "string") {
            type = XParameterType::String;
        } else {
            fail(p, "unknown field type: " + t);
        }
        return {std::move(n), type, p};
    }

    Token token() {
        skip();
        const auto begin = cursor_;
        const auto p = location();
        if (cursor_ == text_.size()) {
            fail(p, "missing value token");
        }
        room(budget_.tokens, 1, budget_.limits.max_total_tokens, p, "tokens");
        ++budget_.tokens;
        Token out;
        out.location = p;
        const char quote = text_[cursor_];
        if (quote == '\"' || quote == '\'') {
            out.quoted = true;
            ++cursor_;
            while (cursor_ < text_.size() && text_[cursor_] != quote) {
                // The bounded subset has no escape convention: reject rather than guess.
                if (text_[cursor_] == '\\') {
                    fail(location(), "string escapes are unsupported");
                }
                budget_.charge(1, location());
                out.decoded += text_[cursor_++];
            }
            if (cursor_ == text_.size()) {
                fail(p, "unterminated quoted string");
            }
            ++cursor_;
            if (cursor_ < text_.size() && !space(text_[cursor_])) {
                fail(location(), "missing token separator");
            }
        } else {
            while (cursor_ < text_.size() && !space(text_[cursor_])) {
                if (text_[cursor_] == '\"' || text_[cursor_] == '\'' || text_[cursor_] == '\\') {
                    fail(location(), "quote or escape inside unquoted token");
                }
                budget_.charge(1, location());
                out.decoded += text_[cursor_++];
            }
        }
        budget_.charge(cursor_ - begin, p);
        out.raw = text_.substr(begin, cursor_ - begin);
        return out;
    }

private:
    const std::string &text_;
    std::size_t number_;
    std::size_t cursor_{};
    TextBudget &budget_;
};

inline std::int64_t integer(const Token &t) {
    const auto &s = t.decoded;
    std::size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i++] == '-';
    }
    if (t.quoted || i == s.size()) {
        fail(t.location, "invalid int64 decimal token");
    }
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto limit = maximum + (negative ? std::uint64_t{1} : std::uint64_t{0});
    std::uint64_t n = 0;
    for (; i < s.size(); ++i) {
        if (!digit(s[i])) {
            fail(t.location, "invalid int64 decimal token");
        }
        const auto d = static_cast<std::uint64_t>(s[i] - '0');
        if (n > (limit - d) / 10) {
            fail(t.location, "int64 out of range");
        }
        n = n * 10 + d;
    }
    if (negative && n == maximum + std::uint64_t{1}) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return negative ? -static_cast<std::int64_t>(n) : static_cast<std::int64_t>(n);
}

inline double real(const Token &t) {
    const auto &s = t.decoded;
    std::size_t i = 0;
    bool nonzero = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        ++i;
    }
    const auto digits = [&] {
        const auto start = i;
        while (i < s.size() && digit(s[i])) {
            nonzero = nonzero || s[i] != '0';
            ++i;
        }
        return i != start;
    };
    bool significand = digits();
    if (i < s.size() && s[i] == '.') {
        ++i;
        significand = digits() || significand;
    }
    const bool nonzero_significand = nonzero;
    bool exponent = true;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        exponent = digits();
    }
    if (t.quoted || !significand || !exponent || i != s.size()) {
        fail(t.location, "invalid decimal real token");
    }
    std::istringstream input(s);
    input.imbue(std::locale::classic());
    double value = 0.;
    input >> value;
    const bool tiny = value != 0. && std::abs(value) <= std::numeric_limits<double>::min();
    if (input.bad() || (input.fail() && !tiny) ||
        input.rdbuf()->sgetc() != std::char_traits<char>::eof() || !std::isfinite(value) ||
        (value == 0. && nonzero_significand)) {
        fail(t.location, "real out of finite double range");
    }
    return value;
}

inline XParameterValue value(Line &line, XParameterType type) {
    auto first = line.token();
    XParameterValue out;
    out.location = first.location;
    out.raw_tokens.push_back(std::move(first.raw));
    if (type == XParameterType::Integer) {
        out.value = integer(first);
    } else if (type == XParameterType::Real) {
        out.value = real(first);
    } else if (type == XParameterType::String) {
        out.value = std::move(first.decoded);
    } else {
        const auto re = real(first);
        auto second = line.token();
        out.value = std::complex<double>(re, real(second));
        out.raw_tokens.push_back(std::move(second.raw));
    }
    return out;
}

class Reader {
public:
    Reader(std::istream &input, XParameterLimits limits)
        : input_(input), limits_(limits), budget_{0, 0, 0, limits_} {
    }

    XParameterDocument read() {
        XParameterDocument doc;
        std::vector<XParameterNamedValue> pending;
        std::set<std::string> names;
        XParameterBlock block;
        bool inside = false, data = false, legacy = false, v2 = false, attributes = false;
        std::size_t columns = 0, declarations = 0;
        XParameterRow row;
        std::string text;
        while (next_line(text)) {
            Line line(text, lines_, budget_);
            if (line.end() || line.peek() == '!' || line.keyword("REM")) {
                continue;
            }
            const auto p = line.location();
            if (!inside) {
                if (line.keyword("VAR")) {
                    auto count = pending.size();
                    add_declaration(count, p);
                    auto f = line.field();
                    unique(names, f);
                    line.symbol('=');
                    add_value(p);
                    auto v = value(line, f.type);
                    require_end(line);
                    pending.push_back({std::move(f), std::move(v)});
                } else if (line.keyword("BEGIN")) {
                    room(doc.blocks_.size(), 1, limits_.max_blocks, p, "blocks");
                    block = {};
                    block.location = p;
                    block.name = line.name();
                    require_end(line);
                    if (block.name == "PHDModelData") {
                        legacy = true;
                    } else if (block.name == "XParamAttributes" || block.name == "XParamPortData" ||
                               block.name == "XParamData") {
                        v2 = true;
                    } else {
                        fail(p, "unsupported block: " + block.name);
                    }
                    if (legacy && v2) {
                        fail(p, "mixed legacy and version-2 blocks");
                    }
                    if (block.name == "XParamAttributes") {
                        if (attributes) {
                            fail(p, "duplicate XParamAttributes block");
                        }
                        attributes = true;
                    }
                    block.variables = std::move(pending);
                    pending.clear();
                    declarations = block.variables.size();
                    columns = 0;
                    row = {};
                    data = false;
                    inside = true;
                } else {
                    fail(p, "expected VAR or BEGIN");
                }
                continue;
            }
            if (line.keyword("END")) {
                require_end(line);
                if (block.headers.empty() || block.rows.empty()) {
                    fail(p, "block requires headers and data");
                }
                if (!row.groups.empty()) {
                    fail(p, "incomplete multiline data group");
                }
                if (block.name == "XParamAttributes") {
                    validate_version(block);
                }
                doc.blocks_.push_back(std::move(block));
                names.clear();
                inside = false;
            } else if (line.peek() == '#') {
                if (data || !block.headers.empty()) {
                    fail(p, "constant must precede headers");
                }
                line.symbol('#');
                add_declaration(declarations, p);
                auto f = line.field();
                unique(names, f);
                line.symbol('=');
                add_value(p);
                auto v = value(line, f.type);
                require_end(line);
                block.constants.push_back({std::move(f), std::move(v)});
            } else if (line.peek() == '%') {
                if (data) {
                    fail(p, "header after data");
                }
                room(block.headers.size(),
                     1,
                     limits_.max_header_groups_per_block,
                     p,
                     "header groups");
                line.symbol('%');
                XParameterHeaderGroup group;
                group.location = p;
                while (!line.end()) {
                    room(columns, 1, limits_.max_columns_per_block, line.location(), "columns");
                    add_declaration(declarations, line.location());
                    auto f = line.field();
                    line.separator();
                    unique(names, f);
                    group.fields.push_back(std::move(f));
                    ++columns;
                }
                if (group.fields.empty()) {
                    fail(p, "empty header group");
                }
                block.headers.push_back(std::move(group));
            } else {
                if (block.headers.empty()) {
                    fail(p, "expected header before data");
                }
                if (row.groups.empty()) {
                    room(block.rows.size(), 1, limits_.max_rows_per_block, p, "rows per block");
                    room(total_rows_, 1, limits_.max_total_rows, p, "total rows");
                    // Check the entire logical row before any of its values are allocated.
                    room(total_values_, columns, limits_.max_total_values, p, "total values");
                }
                data = true;
                XParameterDataLine group;
                group.location = p;
                for (const auto &f : block.headers[row.groups.size()].fields) {
                    add_value(line.location());
                    group.values.push_back(value(line, f.type));
                }
                require_end(line);
                row.groups.push_back(std::move(group));
                if (row.groups.size() == block.headers.size()) {
                    block.rows.push_back(std::move(row));
                    row = {};
                    ++total_rows_;
                }
            }
        }
        const XParameterLocation eof{
            lines_ == std::numeric_limits<std::size_t>::max() ? lines_ : lines_ + 1, 1};
        if (inside) {
            fail(eof, "missing END");
        }
        if (!pending.empty()) {
            fail(pending.front().field.location, "VAR has no following block");
        }
        if (doc.blocks_.empty()) {
            fail(eof, "no blocks");
        }
        if (v2 && !attributes) {
            fail(eof, "version-2 blocks require XParamAttributes.Version");
        }
        doc.version_ = v2 ? XParameterVersion::V2_0 : XParameterVersion::Legacy;
        return doc;
    }

private:
    static void require_end(Line &line) {
        if (!line.end()) {
            fail(line.location(), "unexpected trailing token or declaration");
        }
    }

    void unique(std::set<std::string> &names, const XParameterField &f) {
        budget_.charge(f.name.size(), f.location);
        if (!names.insert(f.name).second) {
            fail(f.location, "duplicate declaration: " + f.name);
        }
    }

    void add_declaration(std::size_t &n, XParameterLocation p) {
        room(n, 1, limits_.max_declarations_per_block, p, "declarations");
        room(budget_.declarations, 1, limits_.max_total_declarations, p, "total declarations");
        ++budget_.declarations;
        ++n;
    }

    void add_value(XParameterLocation p) {
        room(total_values_, 1, limits_.max_total_values, p, "total values");
        ++total_values_;
    }

    static void validate_version(const XParameterBlock &b) {
        if (b.rows.size() != 1) {
            fail(b.location, "XParamAttributes requires one row");
        }
        bool found = false;
        for (std::size_t g = 0; g < b.headers.size(); ++g) {
            for (std::size_t c = 0; c < b.headers[g].fields.size(); ++c) {
                const auto &f = b.headers[g].fields[c];
                if (f.name != "Version") {
                    continue;
                }
                const auto &v = b.rows[0].groups[g].values[c];
                if (f.type != XParameterType::Real || std::get<double>(v.value) != 2.0) {
                    fail(v.location, "only real Version=2.0 is supported");
                }
                found = true;
            }
        }
        if (!found) {
            fail(b.location, "missing Version(real) field");
        }
    }

    std::size_t next_line_number() const {
        return lines_ == std::numeric_limits<std::size_t>::max() ? lines_ : lines_ + 1;
    }

    bool next_line(std::string &out) {
        out.clear();
        bool any = false;
        for (;;) {
            int ch;
            try {
                ch = input_.get();
            } catch (const std::ios_base::failure &) {
                if (input_.eof() && !input_.bad()) {
                    ch = std::char_traits<char>::eof();
                } else {
                    fail({next_line_number(), out.size() + 1}, "input read failure");
                }
            } catch (const std::bad_alloc &) {
                throw;
            } catch (...) {
                if (input_.bad()) {
                    fail({next_line_number(), out.size() + 1}, "input read failure");
                }
                throw;
            }
            if (ch == std::char_traits<char>::eof()) {
                if (input_.bad() || (!input_.eof() && input_.fail())) {
                    fail({next_line_number(), out.size() + 1}, "input read failure");
                }
                if (!any) {
                    return false;
                }
                break;
            }
            const XParameterLocation p{next_line_number(), out.size() + 1};
            room(bytes_, 1, limits_.max_bytes, p, "bytes");
            ++bytes_;
            if (!any) {
                room(lines_, 1, limits_.max_lines, p, "lines");
                any = true;
            }
            if (ch == '\n') {
                break;
            }
            room(out.size(), 1, limits_.max_line_bytes, p, "line bytes");
            if (ch == 0 || (ch < 32 && ch != '\t' && ch != '\r')) {
                fail(p, "unsupported control character");
            }
            out += static_cast<char>(ch);
        }
        ++lines_;
        return true;
    }

    std::istream &input_;
    XParameterLimits limits_;
    TextBudget budget_;
    std::size_t bytes_{}, lines_{}, total_values_{}, total_rows_{};
};
} // namespace xparameter_detail

inline XParameterDocument read_xparameter_data(std::istream &input, XParameterLimits limits = {}) {
    return xparameter_detail::Reader(input, limits).read();
}

inline XParameterDocument read_xparameter_data(const std::filesystem::path &path,
                                               XParameterLimits limits = {}) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw XParameterParseError({1, 1}, "cannot open input file: " + path.string());
    }
    return read_xparameter_data(input, limits);
}
} // namespace rfmodel
