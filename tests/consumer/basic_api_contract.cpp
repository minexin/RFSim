#include <rfmodel/core/interpolation.hpp>
#include <rfmodel/core/linear_solver.hpp>
#include <limits>
#include <stdexcept>

void basic_api_contract_probe(int operation);

int main() {
    // Exceptions thrown by inline SDK functions in a second TU cross the caller boundary.
    for (int operation = 0; operation < 5; ++operation) {
        bool caught = false;
        try {
            basic_api_contract_probe(operation);
        } catch (const std::out_of_range &) {
            caught = operation == 0;
        } catch (const std::invalid_argument &) {
            caught = operation >= 1 && operation <= 3;
        } catch (const std::overflow_error &) {
            caught = operation == 4;
        }
        if (!caught) {
            throw std::runtime_error("installed exception contract mismatch");
        }
        const auto wave = rfmodel::evaluate_two_port({2, {0, 0, 3, 0}}, {1, 2});
        if (wave.b2 != rfmodel::Complex{3, 6}) {
            throw std::runtime_error("recovery after installed failure");
        }
    }
}
