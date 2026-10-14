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
