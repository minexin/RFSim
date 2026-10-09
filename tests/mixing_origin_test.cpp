#include "rfmodel/mixing_origin.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const MixingOrigin a{{7, 1}}, b{{9, -1}, {7, 1}};
    const auto expanded = expand_mixing_origin({a, b}, {2, -1});
    require(expanded == MixingOrigin{{7, -1}, {7, 1}, {9, -1}}, "canonical signed factors");
    require(expand_mixing_origin({b, a}, {-2, 1}) == expanded, "parent order independent");
    const auto conjugated = expand_mixing_origin({expanded}, {-1});
    require(conjugated == MixingOrigin{{7, -1}, {7, 1}, {9, 1}}, "conjugate every factor");
    require(expand_mixing_origin({conjugated}, {-1}) == expanded, "double conjugation");
    require(expand_mixing_origin({a}, {1, 1, -1}).size() == 3, "opposite signs do not cancel");
    MixingOrigin boundary(256, {UINT64_MAX, 1});
    require(expand_mixing_origin({boundary}, {1}).size() == 256, "factor boundary");
    rejects<std::length_error>([&] {
        expand_mixing_origin({boundary, a}, {1, 2});
    });
    rejects<std::invalid_argument>([&] {
        expand_mixing_origin({a}, {0});
    });
    rejects<std::invalid_argument>([&] {
        expand_mixing_origin({a}, {std::numeric_limits<int>::min()});
    });
    rejects<std::invalid_argument>([] {
        expand_mixing_origin({{{0, 1}}}, {1});
    });
    rejects<std::invalid_argument>([] {
        expand_mixing_origin({{{1, 0}}}, {1});
    });
    rejects<std::invalid_argument>([] {
        expand_mixing_origin({{}}, {1});
    });
}
