#pragma once

#include <rfmodel/io/xparameter_data.hpp>

#include <map>
#include <memory>
#include <optional>

namespace rfmodel {
// A restricted raw-data tool contract, not a vendor model/schema conversion.
using XParameterScalar = decltype(XParameterValue::value);

struct XParameterExactTerm {
    std::string name;
    XParameterScalar value;
};
enum class XParameterIndexStatus {
    Parsed,
    UnknownFamily,
    UnsupportedFamily,
    InvalidSyntax,
    IntegerOverflow,
    PortOutOfRange,
    MixingCountMismatch
};

struct XParameterRawIndex {
    XParameterIndexStatus status{XParameterIndexStatus::UnknownFamily};
    std::optional<std::int64_t> port;
    std::vector<std::int64_t> mixing;
};

struct XParameterCatalogField {
    XParameterField field;
    std::string family;
    std::string suffix; // Literal text after the first underscore, without interpretation.
    std::size_t block{}, group{}, column{};
    XParameterRawIndex index;
};

struct XParameterCatalogAttributes {
    double version{};
    std::int64_t highest_port_index{}, fundamental_count{};
};

struct XParameterCatalogPort {
    std::int64_t number{};
    std::optional<XParameterValue> reference;
    std::optional<XParameterValue> name;
    std::size_t block{}, row{};
};

struct XParameterCatalogLimits {
    std::size_t max_fields{1000000};
    std::size_t max_rows{1000000};
    std::size_t max_condition_terms{4000000};
    std::size_t max_ports{100000};
    std::size_t max_mixing_terms{4000000};
};

class XParameterCatalogError : public std::runtime_error {
public:
    explicit XParameterCatalogError(const std::string &message) : std::runtime_error(message) {
    }
};

namespace xparameter_catalog_detail {
struct RowAddress {
    std::size_t block{}, row{};
};

struct State {
    XParameterDocument document;
    XParameterCatalogAttributes attributes;
    std::map<std::int64_t, XParameterCatalogPort> ports;
    std::vector<std::vector<XParameterCatalogField>> fields;
    std::vector<RowAddress> rows;
};

inline void require(bool okay, const std::string &message) {
    if (!okay) {
        throw XParameterCatalogError(message);
    }
}

inline void charge(std::size_t &used, std::size_t n, std::size_t limit, const char *name) {
    require(used <= limit && n <= limit - used, std::string(name) + " limit exceeded");
    used += n;
}

inline const XParameterValue *
named(const XParameterBlock &b, std::size_t row, const std::string &name) {
    for (std::size_t g = 0; g < b.headers.size(); ++g) {
        for (std::size_t c = 0; c < b.headers[g].fields.size(); ++c) {
            if (b.headers[g].fields[c].name == name) {
                return &b.rows[row].groups[g].values[c];
            }
        }
    }
    return nullptr;
}

inline std::int64_t positive(const XParameterValue *v, const char *name) {
    require(v && std::holds_alternative<std::int64_t>(v->value),
            std::string("missing or non-integer ") + name);
    const auto n = std::get<std::int64_t>(v->value);
    require(n > 0, std::string(name) + " must be positive");
    return n;
}

inline XParameterIndexStatus decimal(const std::string &s, bool allow_negative, std::int64_t &out) {
    std::size_t i = 0;
    const bool negative = allow_negative && !s.empty() && s[0] == 'm';
    if (negative) {
        ++i;
    }
    if (i == s.size()) {
        return XParameterIndexStatus::InvalidSyntax;
    }
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto limit = maximum + (negative ? std::uint64_t{1} : std::uint64_t{0});
    std::uint64_t n = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return XParameterIndexStatus::InvalidSyntax;
        }
        const auto d = static_cast<std::uint64_t>(s[i] - '0');
        if (n > (limit - d) / 10) {
            return XParameterIndexStatus::IntegerOverflow;
        }
        n = n * 10 + d;
    }
    out = negative && n == maximum + 1 ? std::numeric_limits<std::int64_t>::min()
          : negative                   ? -static_cast<std::int64_t>(n)
                                       : static_cast<std::int64_t>(n);
    return XParameterIndexStatus::Parsed;
}

inline XParameterRawIndex index(const std::string &name,
                                const std::string &family,
                                const XParameterCatalogAttributes &attributes,
                                std::size_t &mixing_count,
                                std::size_t mixing_limit) {
    XParameterRawIndex result;
    if (family != "AN" && family != "FB") {
        if (family == "S" || family == "T" || family == "XY" || family == "XZ" || family == "AM" ||
            family == "AP" || family == "FI" || family == "FV") {
            result.status = XParameterIndexStatus::UnsupportedFamily;
        }
        return result;
    }
    if (name.size() <= family.size() || name[family.size()] != '_') {
        result.status = XParameterIndexStatus::InvalidSyntax;
        return result;
    }
    std::size_t begin = family.size() + 1, ordinal = 0;
    for (;;) {
        const auto end = name.find('_', begin);
        const auto token = name.substr(begin, end == std::string::npos ? end : end - begin);
        std::int64_t n = 0;
        result.status = decimal(token, ordinal != 0, n);
        if (result.status != XParameterIndexStatus::Parsed) {
            return result;
        }
        if (ordinal == 0) {
            if (n <= 0 || n > attributes.highest_port_index) {
                result.status = XParameterIndexStatus::PortOutOfRange;
                return result;
            }
            result.port = n;
        } else {
            charge(mixing_count, 1, mixing_limit, "mixing terms");
            result.mixing.push_back(n);
        }
        ++ordinal;
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (static_cast<std::uint64_t>(result.mixing.size()) !=
        static_cast<std::uint64_t>(attributes.fundamental_count)) {
        result.status = XParameterIndexStatus::MixingCountMismatch;
    }
    return result;
}

inline std::shared_ptr<const State> build(XParameterDocument document,
                                          XParameterCatalogLimits limits) {
    require(!document.blocks().empty(), "empty or moved-from document");
    require(document.version() == XParameterVersion::V2_0, "catalog supports version 2.0 only");
    auto state = std::make_shared<State>();
    state->document = std::move(document);
    const auto &blocks = state->document.blocks();
    bool found_attributes = false;
    for (const auto &b : blocks) {
        if (b.name == "XParamAttributes" || b.name == "XParamPortData") {
            require(b.variables.empty() && b.constants.empty(), "scoped metadata is unsupported");
        }
        if (b.name == "XParamAttributes") {
            found_attributes = true;
            state->attributes.version = std::get<double>(named(b, 0, "Version")->value);
            state->attributes.highest_port_index = positive(named(b, 0, "NumPorts"), "NumPorts");
            state->attributes.fundamental_count =
                positive(named(b, 0, "NumFundFreqs"), "NumFundFreqs");
        }
    }
    require(found_attributes, "missing XParamAttributes");
    std::size_t fields = 0, rows = 0, conditions = 0, ports = 0, mixing = 0;
    state->fields.resize(blocks.size());
    for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
        const auto &b = blocks[bi];
        if (b.name == "XParamPortData") {
            for (std::size_t r = 0; r < b.rows.size(); ++r) {
                charge(ports, 1, limits.max_ports, "ports");
                const auto p = positive(named(b, r, "PortNumber"), "PortNumber");
                require(p <= state->attributes.highest_port_index, "PortNumber exceeds NumPorts");
                require(state->ports.count(p) == 0, "duplicate PortNumber");
                XParameterCatalogPort port;
                port.number = p;
                port.block = bi;
                port.row = r;
                if (const auto *v = named(b, r, "RefZ0")) {
                    require(std::holds_alternative<std::complex<double>>(v->value),
                            "RefZ0 must be complex");
                    port.reference = *v;
                }
                if (const auto *v = named(b, r, "PortName")) {
                    require(std::holds_alternative<std::string>(v->value),
                            "PortName must be string");
                    port.name = *v;
                }
                state->ports.emplace(p, std::move(port));
            }
        }
        if (b.name != "XParamData") {
            continue;
        }
        for (std::size_t g = 0; g < b.headers.size(); ++g) {
            for (std::size_t c = 0; c < b.headers[g].fields.size(); ++c) {
                charge(fields, 1, limits.max_fields, "fields");
                const auto &f = b.headers[g].fields[c];
                const auto separator = f.name.find('_');
                const auto family = f.name.substr(0, separator);
                const auto suffix =
                    separator == std::string::npos ? std::string{} : f.name.substr(separator + 1);
                state->fields[bi].push_back(
                    {f,
                     family,
                     suffix,
                     bi,
                     g,
                     c,
                     index(f.name, family, state->attributes, mixing, limits.max_mixing_terms)});
            }
        }
        for (std::size_t r = 0; r < b.rows.size(); ++r) {
            charge(rows, 1, limits.max_rows, "rows");
            charge(conditions, b.variables.size(), limits.max_condition_terms, "condition terms");
            charge(conditions, 1, limits.max_condition_terms, "condition terms");
            state->rows.push_back({bi, r});
        }
    }
    return state;
}
} // namespace xparameter_catalog_detail

class XParameterCatalog;

// Each row owns a share of immutable raw storage, independent of the catalog lifetime.
class XParameterCatalogRow {
public:
    std::size_t block_index() const noexcept {
        return address_.block;
    }

    std::size_t row_index() const noexcept {
        return address_.row;
    }

    const std::vector<XParameterCatalogField> &fields() const {
        return state_->fields[address_.block];
    }

    std::vector<XParameterNamedValue> conditions() const {
        const auto &b = state_->document.blocks()[address_.block];
        auto out = b.variables;
        out.push_back({b.headers[0].fields[0], b.rows[address_.row].groups[0].values[0]});
        return out;
    }

    const XParameterValue &value(const std::string &name) const {
        const auto &b = state_->document.blocks()[address_.block];
        const auto *v = xparameter_catalog_detail::named(b, address_.row, name);
        if (!v) {
            throw XParameterCatalogError("raw field not found: " + name);
        }
        return *v;
    }

private:
    friend class XParameterCatalog;

    XParameterCatalogRow(std::shared_ptr<const xparameter_catalog_detail::State> state,
                         xparameter_catalog_detail::RowAddress address)
        : state_(std::move(state)), address_(address) {
    }

    bool matches(const std::vector<XParameterExactTerm> &terms) const {
        const auto &b = state_->document.blocks()[address_.block];
        for (const auto &term : terms) {
            const XParameterValue *v = nullptr;
            for (const auto &variable : b.variables) {
                if (variable.field.name == term.name) {
                    v = &variable.value;
                }
            }
            if (b.headers[0].fields[0].name == term.name) {
                v = &b.rows[address_.row].groups[0].values[0];
            }
            if (!v || v->value != term.value) {
                return false;
            }
        }
        return true;
    }

    std::shared_ptr<const xparameter_catalog_detail::State> state_;
    xparameter_catalog_detail::RowAddress address_;
};
enum class XParameterMatchStatus {
    None,
    One,
    Multiple
};

struct XParameterCatalogMatches {
    std::vector<XParameterCatalogRow> rows;

    XParameterMatchStatus status() const noexcept {
        return rows.empty()       ? XParameterMatchStatus::None
               : rows.size() == 1 ? XParameterMatchStatus::One
                                  : XParameterMatchStatus::Multiple;
    }
};

class XParameterCatalog {
public:
    explicit XParameterCatalog(XParameterDocument document, XParameterCatalogLimits limits = {})
        : state_(xparameter_catalog_detail::build(std::move(document), limits)) {
    }

    const XParameterDocument &raw_document() const {
        return state_->document;
    }

    const XParameterCatalogAttributes &attributes() const {
        return state_->attributes;
    }

    const std::map<std::int64_t, XParameterCatalogPort> &ports() const {
        return state_->ports;
    }

    const std::vector<XParameterCatalogField> &fields(std::size_t block) const {
        return state_->fields.at(block);
    }

    std::size_t row_count() const noexcept {
        return state_->rows.size();
    }

    XParameterCatalogRow row(std::size_t index) const {
        return XParameterCatalogRow(state_, state_->rows.at(index));
    }

    XParameterCatalogMatches filter(const std::vector<XParameterExactTerm> &terms) const {
        std::set<std::string> names;
        for (const auto &term : terms) {
            if (const auto *v = std::get_if<double>(&term.value)) {
                xparameter_catalog_detail::require(std::isfinite(*v),
                                                   "nonfinite exact filter term");
            }
            if (const auto *v = std::get_if<std::complex<double>>(&term.value)) {
                xparameter_catalog_detail::require(std::isfinite(v->real()) &&
                                                       std::isfinite(v->imag()),
                                                   "nonfinite exact filter term");
            }
        }
        for (const auto &term : terms) {
            xparameter_catalog_detail::require(names.insert(term.name).second,
                                               "duplicate exact filter term: " + term.name);
        }
        XParameterCatalogMatches result;
        for (const auto &address : state_->rows) {
            XParameterCatalogRow candidate(state_, address);
            if (candidate.matches(terms)) {
                result.rows.push_back(std::move(candidate));
            }
        }
        return result;
    }

    XParameterCatalogRow select_one(const std::vector<XParameterExactTerm> &terms) const {
        auto result = filter(terms);
        if (result.status() == XParameterMatchStatus::None) {
            throw XParameterCatalogError("exact selection: no match");
        }
        if (result.status() == XParameterMatchStatus::Multiple) {
            throw XParameterCatalogError("exact selection: ambiguous");
        }
        return result.rows.front();
    }

private:
    std::shared_ptr<const xparameter_catalog_detail::State> state_;
};
} // namespace rfmodel
