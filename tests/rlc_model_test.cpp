#include "rfmodel/rlc_model.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    for (const auto connection :
         {LumpedConnection::SeriesImpedance, LumpedConnection::ShuntAdmittance}) {
        const bool series = connection == LumpedConnection::SeriesImpedance;
        for (const auto element :
             {IdealElement::Resistor, IdealElement::Inductor, IdealElement::Capacitor}) {
            const double value = element == IdealElement::Resistor
                                     ? 75.
                                     : (element == IdealElement::Inductor ? 2e-9 : 3e-12);
            IdealRLCModel model("element", element, connection, value, 75.);
            for (double f : {1e3, 1e9, 1e12}) {
                const double omega = 2 * 3.14159265358979323846 * f;
                const Complex z =
                    element == IdealElement::Resistor
                        ? Complex{value, 0.}
                        : (element == IdealElement::Inductor ? Complex{0., omega * value}
                                                             : Complex{0., -1. / (omega * value)});
                const auto u = series ? z / 75. : 75. / z;
                const auto s = model.s_parameters(f);
                near(s(0, 0), (series ? 1. : -1.) * u / (2. + u));
                near(s(1, 0), 2. / (2. + u));
                near(s(0, 1), s(1, 0));
                near(s(1, 1), s(0, 0));
                if (element != IdealElement::Resistor) {
                    near(std::norm(s(0, 0)) + std::norm(s(1, 0)), 1.);
                }
            }
            if (element != IdealElement::Resistor) {
                const bool blocked = series ? (element == IdealElement::Capacitor)
                                            : (element == IdealElement::Inductor);
                near(model.s_parameters(0)(1, 0), blocked ? 0. : 1.);
                near(model.s_parameters(0)(0, 0), blocked ? (series ? 1. : -1.) : 0.);
            }
            near(model.port(1).reference_impedance, 75.);
            rejects<std::invalid_argument>([&] {
                model.s_parameters(-1);
            });
        }
    }
    IdealRLCModel extreme("L", IdealElement::Inductor, LumpedConnection::SeriesImpedance, 1e300);
    near(extreme.s_parameters(1e300)(0, 0), 1.);
    near(extreme.s_parameters(1e300)(1, 0), 0.);
    rejects<std::invalid_argument>([] {
        IdealRLCModel bad("C", IdealElement::Capacitor, LumpedConnection::SeriesImpedance, 0.);
    });
}
