// Read-only shape/sample probe; no simulation or vendor compatibility assertion.
#include <rfmodel/io/xparameter_data.hpp>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>

namespace {
void print_value(const rfmodel::XParameterValue &v) {
    for (std::size_t i = 0; i < v.raw_tokens.size(); ++i) {
        if (i) {
            std::cout << ' ';
        }
        std::cout << v.raw_tokens[i];
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: probe-xparameter-data <local-file>\n";
        return 2;
    }
    try {
        const auto doc = rfmodel::read_xparameter_data(std::filesystem::path(argv[1]));
        std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
        std::cout << "Raw typed text only; numerical model compatibility is NOT established.\n"
                  << "version="
                  << (doc.version() == rfmodel::XParameterVersion::Legacy ? "legacy" : "2.0")
                  << '\n';
        std::map<std::string, std::pair<double, double>> ranges;
        for (std::size_t bi = 0; bi < doc.blocks().size(); ++bi) {
            const auto &b = doc.blocks()[bi];
            std::size_t columns = 0;
            for (const auto &h : b.headers) {
                columns += h.fields.size();
            }
            std::cout << "block=" << bi << " name=" << b.name << " rows=" << b.rows.size()
                      << " columns=" << columns << " groups=" << b.headers.size() << '\n';
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
                    if (samples < 5 &&
                        (f.name.rfind("FI_", 0) == 0 || f.name.rfind("FB_2_", 0) == 0 ||
                         f.name.rfind("S_2_", 0) == 0 || f.name.rfind("T_2_", 0) == 0)) {
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
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
