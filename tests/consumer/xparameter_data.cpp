#include <rfmodel/io/xparameter_data.hpp>
#include <sstream>
#include <type_traits>

rfmodel::XParameterDocument xparameter_data_probe();

int main() {
    static_assert(std::is_copy_constructible<rfmodel::XParameterDocument>::value,
                  "owning copyable document");
    const auto d = xparameter_data_probe();
    const auto &b = d.blocks().at(0);
    if (d.version() != rfmodel::XParameterVersion::Legacy || b.name != "PHDModelData" ||
        b.rows.at(0).groups.at(0).values.at(0).raw_tokens.at(0) != "-2.5") {
        return 1;
    }
    std::istringstream input("BEGIN PHDModelData\n% x(int)\n-9223372036854775808\nEND\n");
    const auto local = rfmodel::read_xparameter_data(input);
    return std::get<std::int64_t>(local.blocks()[0].rows[0].groups[0].values[0].value) ==
                   std::numeric_limits<std::int64_t>::min()
               ? 0
               : 2;
}
