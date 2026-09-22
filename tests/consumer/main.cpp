#include <rfmodel/linear_analysis.hpp>
#include <rfmodel/interpolation.hpp>
#include <rfmodel/measurements.hpp>
#include <rfmodel/noise.hpp>
#include <rfmodel/linear_solver.hpp>
#include <cmath>
int main() {
    auto result=rfmodel::analyze_linear(rfmodel::FrequencyGrid{{1e9}}, {0,1}, [](double) {
        rfmodel::LinearNetwork network;
        network.add(rfmodel::SMatrix{2,{0.,0.5,0.5,0.}});
        return network;
    });
    return std::abs(result.scattering[0](1,0)-rfmodel::Complex{0.5,0})<1e-12 ? 0 : 1;
}
