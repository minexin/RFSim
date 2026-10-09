#include "rfmodel/origin_expression.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const OriginExpression a{{{{7, 1}}, {.25, .1}}, {{{9, 1}}, {-.05, .2}}};
    const Complex wave = origin_expression_amplitude(a);
    for (int order = 1; order <= 9; ++order) {
        const auto product = product_origin_expressions({a}, std::vector<int>(order, 1));
        require(product.size() == static_cast<std::size_t>(order + 1), "binomial term count");
        require(std::abs(origin_expression_amplitude(product) - std::pow(wave, order)) < 1e-14,
                "independent complex power oracle");
    }
    const auto mixed = product_origin_expressions({a}, {1, -1}, {2., -3.});
    require(std::abs(origin_expression_amplitude(mixed) - Complex(2., -3.) * std::norm(wave)) <
                1e-14,
            "conjugated product and complex coefficient");
    for (const auto &term : mixed) {
        require(term.factors.size() == 2, "opposite signs retain source order");
    }
    const OriginExpression cancelled{{{{7, 1}}, 1.}, {{{9, 1}}, -1.}};
    const auto square = product_origin_expressions({cancelled}, {1, 1});
    require(square.size() == 3 && origin_expression_amplitude(square) == Complex{},
            "zero total retains monomials");
    require(square[0].amplitude == Complex(1.) && square[1].amplitude == Complex(-2.) &&
                square[2].amplitude == Complex(1.),
            "binomial cross multiplicity");
    const auto zero = sum_origin_expressions({{{{{7, 1}}, 1.}, {{{7, 1}}, -1.}}});
    require(zero.size() == 1 && zero[0].amplitude == Complex{}, "known zero origin retained");
    const auto compensated =
        sum_origin_expressions({{{{{7, 1}}, 1e16}, {{{7, 1}}, 1.}, {{{7, 1}}, -1e16}}});
    require(compensated[0].amplitude == Complex(1.), "compensated coherent addition");
    const auto canonical =
        sum_origin_expressions({{{{{9, -1}, {7, 1}}, 2.}}, {{{{7, 1}, {9, -1}}, 3.}}});
    require(canonical.size() == 1 && canonical[0].amplitude == Complex(5.),
            "factor permutation merges");
    const auto conjugated = product_origin_expressions({canonical}, {-1});
    require(conjugated[0].factors == MixingOrigin{{7, -1}, {9, 1}}, "all factor signs conjugated");
    const auto cube =
        product_origin_expressions({product_origin_expressions({a}, {1, 1}), a}, {1, 2});
    require(std::abs(origin_expression_amplitude(cube) - std::pow(wave, 3)) < 1e-14,
            "recursive products");
    require(sum_origin_expressions({}).empty(), "empty sum");
    require(product_origin_expressions({a, {}}, {1, 2}).empty(), "empty parent is zero");
    rejects<std::invalid_argument>([&] {
        product_origin_expressions({a}, {});
    });
    rejects<std::invalid_argument>([&] {
        product_origin_expressions({a}, {0});
    });
    rejects<std::invalid_argument>([&] {
        product_origin_expressions({a}, {std::numeric_limits<int>::min()});
    });
    rejects<std::invalid_argument>([&] {
        product_origin_expressions({a}, std::vector<int>(10, 1));
    });
    rejects<std::invalid_argument>([] {
        sum_origin_expressions({{{{{0, 1}}, 1.}}});
    });
    rejects<std::invalid_argument>([] {
        sum_origin_expressions({{{{{1, 0}}, 1.}}});
    });
    rejects<std::invalid_argument>([] {
        sum_origin_expressions({{{{}, 1.}}});
    });
    rejects<std::invalid_argument>([] {
        sum_origin_expressions({{{{{1, 1}}, std::numeric_limits<double>::infinity()}}});
    });
    rejects<std::invalid_argument>([&] {
        product_origin_expressions({a}, {1}, {1e308, 0.});
    });
    const OriginExpression boundary{{MixingOrigin(256, {1, 1}), 1.}};
    require(product_origin_expressions({boundary}, {1})[0].factors.size() == 256,
            "factor limit accepted");
    rejects<std::length_error>([&] {
        product_origin_expressions({boundary}, {1, 1});
    });
    OriginExpression many;
    for (std::uint64_t i = 1; i <= 91; ++i) {
        many.push_back({{{i, 1}}, .01});
    }
    rejects<std::length_error>([&] {
        product_origin_expressions({many}, {1, 1});
    });
    OriginExpression factors;
    for (std::uint64_t i = 1; i <= 257; ++i) {
        factors.push_back({MixingOrigin(256, {i, 1}), .01});
    }
    rejects<std::length_error>([&] {
        sum_origin_expressions({factors});
    });
    OriginExpression input_limit(4097, {{{1, 1}}, 0.});
    rejects<std::invalid_argument>([&] {
        sum_origin_expressions({input_limit});
    });
}
