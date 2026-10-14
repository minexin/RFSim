#include <rfmodel/core/interpolation.hpp>
#include <rfmodel/core/linear_solver.hpp>
#include <limits>

void basic_api_contract_probe(int operation) {
    using namespace rfmodel;
    switch (operation) {
    case 0: {
        const SMatrix s{std::numeric_limits<std::size_t>::max() / 2 + 1, {42}};
        (void)s(2, 0);
        break;
    }
    case 1:
        evaluate_two_port({2, {0, 0, 1, 0, 5}}, 1);
        break;
    case 2: {
        TouchstoneData d;
        d.ports = 1;
        d.frequencies_hz = {1};
        d.matrices = {{1, {0}}};
        interpolate_s(d, 1, static_cast<OutOfBand>(42));
        break;
    }
    case 3:
        StaticSParameterModel("", {{0, ""}}, {1, {std::numeric_limits<double>::infinity()}});
        break;
    case 4:
        evaluate_two_port({2, {0, 0, std::numeric_limits<double>::max(), 0}}, 2);
        break;
    }
}
