#include "rfmodel/device_model.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    SMatrix s{2, {0.0, 0.5, 0.5, 0.0}};
    StaticSParameterModel model("attenuator", {{0, "in"}, {1, "out"}}, s);
    require(model.port_count() == 2, "port count");
    near(model.s_parameters(1e9)(1, 0), 0.5);
    rejects<std::invalid_argument>([] {
        StaticSParameterModel bad("bad", {{0, "in"}}, SMatrix{2, {0.0}});
    });
    rejects<std::out_of_range>([&] {
        (void)s(0, 2);
    });
    rejects<std::out_of_range>([&] {
        (void)s(2, 0);
    });
}
