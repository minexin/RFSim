#include "workflow.hpp"
#include <exception>
#include <iostream>
int main() {
    try {
        behavioral_example::verify_and_report(std::cout);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
