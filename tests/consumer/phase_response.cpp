#include <rfmodel/conversion/conversion_phase_response.hpp>
#include <iostream>

rfmodel::ConversionNoise phase_response_odr_probe();

int main() {
    try {
        using namespace rfmodel;
        const auto q = phase_response_group(
            {{0, 9}, {0, 10}, {0, 11}}, {{1, 1.}}, 1., {{1, 1., 2e-10, {{0., 1.}}}});
        const auto r = phase_response_odr_probe();
        for (std::size_t i = 0; i < 9; ++i) {
            if (std::abs(q.covariance.values[i] - r.covariance.values[i]) > 1e-24 ||
                std::abs(q.complementary.values[i] - r.complementary.values[i]) > 1e-24) {
                throw std::runtime_error("installed phase response C/P mismatch");
            }
        }
        if (std::abs(q.complementary(0, 2) + 1e-10) > 1e-24) {
            throw std::runtime_error("installed phase response paired PM mismatch");
        }
        std::cout << "installed complex phase response two-TU consumer passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
