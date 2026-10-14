#include <rfmodel/core/network_parameters.hpp>
#include <cmath>
#include <iostream>

rfmodel::SMatrix make_network();

int main() {
    const auto network = make_network();
    if (std::abs(network(1, 0) - std::complex<double>{0.5, 0.0}) > 1e-12) {
        return 1;
    }
    std::cout << "Matched two-port voltage-wave transmission: " << network(1, 0) << '\n';
    return 0;
}
