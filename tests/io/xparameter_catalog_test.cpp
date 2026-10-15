#include <rfmodel/io/xparameter_catalog.hpp>
#include "../support/test_support.hpp"
#include <array>
#include <iostream>
#include <sstream>

namespace {
using namespace rfmodel;

XParameterDocument parse(const std::string &text) {
    std::istringstream input(text);
    return read_xparameter_data(input);
}

const std::string attributes =
    "BEGIN XParamAttributes\n% NumFundFreqs(int)\n% NumPorts(int) Version(real)\n2\n3 2.0\nEND\n";
const std::string ports =
    "BEGIN XParamPortData\n% PortName(string)\n% RefZ0(complex) PortNumber(int)\n"
    "\"negative reference\"\n-50 7 3\n\"zero reference\"\n0 -2 1\nEND\n";

std::string data(double outer, bool reverse = false) {
    std::ostringstream out;
    out << "VAR AN_1_1_0(real) = " << outer << "\nVAR mystery(int) = 7\n"
        << "VAR label(string) = \"arbitrary variable\"\nVAR phasor(complex) = 2 -3\n"
        << "BEGIN XParamData\n% AN_2_0_1(real)\n";
    if (reverse) {
        out << "% Unknown_thing(string)\n% FB_3_1_m2(complex)\n"
            << "0\nraw\n-2.5 3.5\n1\nother\n4 -5\nEND\n";
    } else {
        out << "% FB_3_1_m2(complex) Unknown_thing(string)\n"
            << "-0\n-2.5 3.5 raw\n1\n4 -5 other\nEND\n";
    }
    return out.str();
}

std::string fixture() {
    return ports + data(10) + attributes + data(20, true);
}

void addresses_and_selection() {
    const XParameterCatalog catalog(parse(fixture()));
    require(catalog.attributes().version == 2.0 && catalog.attributes().fundamental_count == 2 &&
                catalog.attributes().highest_port_index == 3,
            "reordered metadata");
    require(catalog.row_count() == 4, "outer AN times inner AN Cartesian rows");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto row = catalog.row(i);
        require(row.block_index() == (i < 2 ? 1u : 3u) && row.row_index() == i % 2,
                "independent raw row addresses");
        const auto &block = catalog.raw_document().blocks().at(row.block_index());
        require(row.fields().size() == 3, "list all raw fields");
        for (const auto &field : row.fields()) {
            const auto &raw =
                block.rows.at(row.row_index()).groups.at(field.group).values.at(field.column);
            require(field.block == row.block_index() &&
                        block.headers.at(field.group).fields.at(field.column).name ==
                            field.field.name,
                    "field address resolves header");
            require(&raw == &row.value(field.field.name),
                    "named lookup aliases exact owned raw value");
            require(raw.value == row.value(field.field.name).value &&
                        raw.raw_tokens == row.value(field.field.name).raw_tokens,
                    "raw values and lexical tokens preserved");
        }
        const auto conditions = row.conditions();
        require(conditions.size() == 5 && conditions.front().field.name == "AN_1_1_0" &&
                    conditions.back().field.name == "AN_2_0_1",
                "unknown VAR conditions retained");
        require(std::get<std::int64_t>(conditions[1].value.value) == 7 &&
                    std::get<std::string>(conditions[2].value.value) == "arbitrary variable" &&
                    std::get<std::complex<double>>(conditions[3].value.value) ==
                        std::complex<double>(2, -3),
                "heterogeneous condition types retained");
    }
    const auto row = catalog.select_one({{"AN_1_1_0", 20.0}, {"AN_2_0_1", 0.0}});
    require(row.block_index() == 3 && row.fields()[2].group == 2 &&
                std::get<std::complex<double>>(row.value("FB_3_1_m2").value) ==
                    std::complex<double>(-2.5, 3.5),
            "reordered header groups select by exact names");
    require(catalog.filter({{"mystery", std::int64_t{7}}}).rows.size() == 4 &&
                catalog.filter({{"mystery", 7.0}}).status() == XParameterMatchStatus::None,
            "exact typed integer distinct from real");
    require(catalog.filter({{"AN_2_0_1", -0.0}}).rows.size() == 2 &&
                catalog.filter({{"AN_2_0_1", 0.0}}).rows.size() == 2,
            "signed zero equality");
    require(catalog.filter({{"missing", 0.0}}).status() == XParameterMatchStatus::None &&
                catalog.filter({{"FB_3_1_m2", std::complex<double>(-2.5, 3.5)}}).rows.empty(),
            "unknown keys and dependent fields are not conditions");
    require(catalog.filter({}).status() == XParameterMatchStatus::Multiple,
            "empty filter lists all");
    rejects<XParameterCatalogError>([&] {
        (void)catalog.select_one({});
    });
    rejects<XParameterCatalogError>([&] {
        (void)catalog.select_one({{"missing", 1.0}});
    });
    rejects<XParameterCatalogError>([&] {
        (void)row.value("missing");
    });
    rejects<XParameterCatalogError>([&] {
        (void)catalog.filter({{"mystery", std::int64_t{7}}, {"mystery", std::int64_t{7}}});
    });
    const double nan = std::numeric_limits<double>::quiet_NaN();
    rejects<XParameterCatalogError>([&] {
        (void)catalog.filter({{"missing", nan}});
    });
    rejects<XParameterCatalogError>([&] {
        (void)catalog.filter({{"phasor", std::complex<double>(0, nan)}});
    });
    require(catalog.ports().size() == 2 && !catalog.ports().count(2), "sparse port map");
    require(std::get<std::complex<double>>(catalog.ports().at(3).reference->value) ==
                    std::complex<double>(-50, 7) &&
                std::get<std::complex<double>>(catalog.ports().at(1).reference->value) ==
                    std::complex<double>(0, -2),
            "nonpositive complex references retained without interpretation");
}

void indexes() {
    struct Case {
        const char *name;
        XParameterIndexStatus status;
    };

    const Case cases[] = {
        {"FB_3_1_m2", XParameterIndexStatus::Parsed},
        {"AN_1_m9223372036854775808_9223372036854775807", XParameterIndexStatus::Parsed},
        {"FB_4_1_0", XParameterIndexStatus::PortOutOfRange},
        {"FB_0_1_0", XParameterIndexStatus::PortOutOfRange},
        {"FB_9223372036854775808_1_0", XParameterIndexStatus::IntegerOverflow},
        {"FB_1_m9223372036854775809_0", XParameterIndexStatus::IntegerOverflow},
        {"FB_1_9223372036854775808_0", XParameterIndexStatus::IntegerOverflow},
        {"FB_1_0", XParameterIndexStatus::MixingCountMismatch},
        {"FB_1_0_1_2", XParameterIndexStatus::MixingCountMismatch},
        {"FB_1_m_0", XParameterIndexStatus::InvalidSyntax},
        {"FB_1__0", XParameterIndexStatus::InvalidSyntax},
        {"FB_m1_0_0", XParameterIndexStatus::InvalidSyntax},
        {"FB", XParameterIndexStatus::InvalidSyntax},
        {"NEW_3_0", XParameterIndexStatus::UnknownFamily},
        {"S_2_1_1_1", XParameterIndexStatus::UnsupportedFamily},
        {"T_2_1_1_1", XParameterIndexStatus::UnsupportedFamily},
        {"XY_3_0_0", XParameterIndexStatus::UnsupportedFamily},
        {"XZ_3_0_0", XParameterIndexStatus::UnsupportedFamily}};
    for (const auto &item : cases) {
        XParameterCatalog c(parse(attributes + "BEGIN XParamData\n% sweep(real) " + item.name +
                                  "(complex)\n1 -2 3\nEND\n"));
        const auto row = c.row(0);
        const auto &field = row.fields().at(1);
        require(field.index.status == item.status, item.name);
        require(std::get<std::complex<double>>(c.row(0).value(item.name).value) ==
                    std::complex<double>(-2, 3),
                "unsupported index never drops raw field");
        if (std::string(item.name) == "FB_3_1_m2") {
            require(field.suffix == "3_1_m2" && field.index.port == 3 &&
                        field.index.mixing == std::vector<std::int64_t>({1, -2}),
                    "negative mixing independently expected");
        }
        if (std::string(item.name).find("AN_1_m") == 0) {
            require(field.index.mixing ==
                        std::vector<std::int64_t>({std::numeric_limits<std::int64_t>::min(),
                                                   std::numeric_limits<std::int64_t>::max()}),
                    "signed mixing boundaries");
        }
    }
}

void metadata_and_ownership() {
    const std::string minimal = "BEGIN XParamData\n% x(real)\n1\nEND\n";
    for (const auto &meta :
         {std::string("BEGIN XParamAttributes\n% Version(real)\n2.0\nEND\n"),
          std::string("BEGIN XParamAttributes\n% Version(real) NumPorts(int) NumFundFreqs(int)\n2 "
                      "0 2\nEND\n"),
          std::string("BEGIN XParamAttributes\n% Version(real) NumPorts(int) NumFundFreqs(real)\n2 "
                      "3 2\nEND\n"),
          std::string("VAR scope(real) = 1\n") + attributes,
          std::string("BEGIN XParamAttributes\n# scope(real) = 1\n% Version(real) NumPorts(int) "
                      "NumFundFreqs(int)\n2 3 2\nEND\n")}) {
        rejects<XParameterCatalogError>([&] {
            XParameterCatalog c(parse(meta + minimal));
        });
    }
    rejects<XParameterCatalogError>([&] {
        XParameterCatalog c(parse(attributes + ports + ports + minimal));
    });
    rejects<XParameterCatalogError>([&] {
        XParameterCatalog c(parse(attributes + "VAR scope(real) = 1\n" + ports + minimal));
    });
    rejects<XParameterCatalogError>([] {
        XParameterCatalog c(XParameterDocument{});
    });
    rejects<XParameterCatalogError>([] {
        XParameterCatalog c(parse("BEGIN PHDModelData\n% x(real)\n1\nEND\n"));
    });
    const XParameterCatalog sparse(
        parse("BEGIN XParamAttributes\n% Version(real) NumPorts(int) NumFundFreqs(int)\n2 "
              "9223372036854775807 2\nEND\n"
              "BEGIN XParamPortData\n% PortNumber(int)\n9223372036854775807\nEND\n" +
              minimal));
    require(sparse.ports().size() == 1 && !sparse.ports().begin()->second.reference &&
                !sparse.ports().begin()->second.name,
            "huge sparse port count and optional metadata");
    const XParameterCatalog huge_fundamentals(
        parse("BEGIN XParamAttributes\n% Version(real) NumPorts(int) NumFundFreqs(int)\n2 3 "
              "9223372036854775807\nEND\n"
              "BEGIN XParamData\n% AN_1_0(real)\n0\nEND\n"));
    const auto huge_row = huge_fundamentals.row(0);
    require(huge_row.fields()[0].index.status == XParameterIndexStatus::MixingCountMismatch &&
                huge_row.fields()[0].index.mixing == std::vector<std::int64_t>({0}),
            "huge fundamental metadata does not allocate absent mixing terms");
    for (const auto &port_data :
         {"BEGIN XParamPortData\n% PortNumber(int)\n0\nEND\n",
          "BEGIN XParamPortData\n% PortNumber(int)\n4\nEND\n",
          "BEGIN XParamPortData\n% PortNumber(real)\n1\nEND\n",
          "BEGIN XParamPortData\n% PortName(string)\nmissing\nEND\n",
          "BEGIN XParamPortData\n# scope(real) = 1\n% PortNumber(int)\n1\nEND\n"}) {
        rejects<XParameterCatalogError>([&] {
            XParameterCatalog c(parse(attributes + port_data + minimal));
        });
    }
    for (const auto &port_data :
         {"BEGIN XParamPortData\n% PortNumber(int)\n1\n1\nEND\n",
          "BEGIN XParamPortData\n% PortNumber(int) RefZ0(real)\n1 50\nEND\n",
          "BEGIN XParamPortData\n% PortNumber(int) PortName(int)\n1 2\nEND\n"}) {
        rejects<XParameterCatalogError>([&] {
            XParameterCatalog c(parse(attributes + port_data + minimal));
        });
    }
    rejects<XParameterParseError>([&] {
        (void)parse(attributes + "VAR x(real) = 1\n" + minimal);
    });
    const XParameterCatalog heterogeneous(parse(attributes + "VAR fund_1(real) = 100\n" + minimal +
                                                "VAR mystery(string) = odd\n" + minimal + minimal));
    require(heterogeneous.row(0).conditions().size() == 2 &&
                heterogeneous.row(1).conditions().size() == 2 &&
                heterogeneous.row(2).conditions().size() == 1,
            "heterogeneous block scopes have no inherited VAR conditions");
    require(
        heterogeneous.filter({{"fund_1", 100.0}}).rows.size() == 1 &&
            heterogeneous.filter({{"mystery", std::string("odd")}}).rows.size() == 1 &&
            heterogeneous.filter({{"fund_1", 100.0}, {"mystery", std::string("odd")}}).rows.empty(),
        "missing heterogeneous keys never match another block's values");
    const auto retained = [] {
        auto source = parse(fixture());
        XParameterCatalog original(source);
        auto copy = original;
        auto moved = std::move(copy);
        return moved.row(0);
    }();
    auto copied_row = retained;
    auto moved_row = std::move(copied_row);
    require(moved_row.value("FB_3_1_m2").raw_tokens == std::vector<std::string>({"-2.5", "3.5"}) &&
                retained.fields().size() == 3,
            "rows own storage across source/catalog copies destruction and moves");
    auto source = parse(fixture());
    auto moved_source = std::move(source);
    require(!moved_source.blocks().empty(), "move source populated");
    rejects<XParameterCatalogError>([&] {
        XParameterCatalog c(source);
    });
    const XParameterCatalog duplicates(parse(attributes + data(10) + data(10)));
    require(duplicates.filter({{"AN_1_1_0", 10.0}, {"AN_2_0_1", 0.0}}).status() ==
                XParameterMatchStatus::Multiple,
            "duplicate exact conditions retain all rows");
    rejects<XParameterCatalogError>([&] {
        (void)duplicates.select_one({{"AN_1_1_0", 10.0}, {"AN_2_0_1", 0.0}});
    });
}

void limits() {
    // Actual stored counts: 2 data blocks x 3 fields; 4 data rows;
    // each row has 4 VAR terms + 1 inner term; 2 sparse port rows;
    // each block has AN and FB with 2 mixing terms each.
    const std::array<std::pair<std::size_t XParameterCatalogLimits::*, std::size_t>, 5> counts{
        {{&XParameterCatalogLimits::max_fields, 6},
         {&XParameterCatalogLimits::max_rows, 4},
         {&XParameterCatalogLimits::max_condition_terms, 20},
         {&XParameterCatalogLimits::max_ports, 2},
         {&XParameterCatalogLimits::max_mixing_terms, 8}}};
    XParameterCatalogLimits none;
    none.max_mixing_terms = 0;
    const XParameterCatalog unknown(
        parse(attributes + "BEGIN XParamData\n% custom_1_2_3(real)\n0\nEND\n"), none);
    require(unknown.row_count() == 1, "unknown family consumes no parsed mixing terms");
    for (const auto &entry : counts) {
        XParameterCatalogLimits l;
        l.*entry.first = entry.second;
        require(XParameterCatalog(parse(fixture()), l).row_count() == 4,
                "exact actual-count limit accepted");
        --(l.*entry.first);
        rejects<XParameterCatalogError>([&] {
            XParameterCatalog c(parse(fixture()), l);
        });
        l.*entry.first = 0;
        rejects<XParameterCatalogError>([&] {
            XParameterCatalog c(parse(fixture()), l);
        });
    }
}
} // namespace

int main() {
    try {
        addresses_and_selection();
        indexes();
        metadata_and_ownership();
        limits();
        std::cout << "xparameter catalog independent contract passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
