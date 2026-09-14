#include "rfmodel/linear_solver.hpp"
#include "test_support.hpp"
int main() {
    using namespace rfmodel;
    // Reciprocal matched attenuator: half wave amplitude, quarter power.
    auto w = evaluate_two_port(SMatrix{2,{0.0,0.5,0.5,0.0}}, 1.0);
    near(w.b1, 0.0);
    near(w.b2, 0.5);
    require(std::abs(std::norm(w.b2)-0.25) < 1e-12, "power transmission");
    // Nonreciprocal matrix checks wave direction and simultaneous excitations.
    w = evaluate_two_port(SMatrix{2,{0.1,Complex{0,0.2},2.0,0.3}},
                          Complex{1,1}, Complex{0,1});
    near(w.b1, Complex{-0.1,0.1});
    near(w.b2, Complex{2.0,2.3});
    rejects<std::invalid_argument>([] {
        evaluate_two_port(SMatrix{1,{0.0}}, 1.0);
    });
}
