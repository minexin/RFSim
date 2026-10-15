#include <rfmodel/io/xparameter_catalog.hpp>
#include <type_traits>

rfmodel::XParameterCatalog xparameter_catalog_probe();

int main() {
    static_assert(std::is_copy_constructible<rfmodel::XParameterCatalogRow>::value,
                  "owning rows can be copied across translation units");
    auto catalog = xparameter_catalog_probe();
    if (catalog.row(0).conditions().size() != 2) {
        return 4;
    }
    if (catalog.row_count() != 2 || catalog.fields(1).size() != 2) {
        return 1;
    }
    const auto row = catalog.select_one({{"AN_1_1_0", 10.0}, {"AN_2_0_1", 0.0}});
    const auto &field = row.fields().at(1);
    if (field.field.name != "FB_3_1_m2" || field.suffix != "3_1_m2" || field.index.port != 3 ||
        field.index.mixing != std::vector<std::int64_t>({1, -2})) {
        return 2;
    }
    return std::get<std::complex<double>>(row.value(field.field.name).value) ==
                   std::complex<double>(-2.5, 3.5)
               ? 0
               : 3;
}
