#include <rfmodel/adapters/builtin_model_adapters.hpp>
#include <rfmodel/adapters/systemvue2023_am_pm.hpp>

rfmodel::ModelAdapter make_installed_adapter() {
    return rfmodel::adapt_polynomial_amplifier(
        rfmodel::MatchedPolynomialAmplifier("owned", {0., 2.}),
        {"consumer.polynomial", "1", "", ""});
}

double installed_vendor_phase() {
    return rfmodel::SystemVue2023PointDerivativeAmPm(-5., 10.).response(.01).phase_rad;
}

rfmodel::ModelAdapter make_installed_highorder_adapter() {
    return rfmodel::adapt_highorder_amplifier(
        "installed high-order", {20., 20., 23., {.1}, 75.}, {1});
}

rfmodel::ConversionNonlinearDevice make_installed_bilinear_device() {
    auto adapter = rfmodel::adapt_bilinear_mixer("owned mixer", 0., 2., 75.);
    auto copy = adapter;
    return copy.operating_device(3, 1e6, {{2, 2}, {1, 10}, {2, 22}, {0, 12}});
}
