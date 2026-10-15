#include "frozen_model.hpp"
#include <exception>
#include <iostream>

int main() {
    try {
        frozen_example::verify_workflows();
        std::cout << "PASS: owning frozen DC adapter; bias 0.8/0.4 V; full S/Q oracle at "
                     "DC, 1 MHz, 10 MHz; linear/noise/conversion/spectrum/coherent; "
                     "missing versus explicit zero noise; source callbacks destroyed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
