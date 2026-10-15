// A real second translation unit for both source and relocated SDK consumers.
// Only public includes are allowed; no implementation/source-tree fallback.
#include <rfmodel/nonlinear/fundamental_compression.hpp>
#include <rfmodel/nonlinear/polynomial_amplifier.hpp>
#include <cmath>
#include <stdexcept>

void rfmodel_reference_amplifier_odr_check() {
    const rfmodel::Complex wave{.001, -.002};
    const rfmodel::P1dBFundamentalCompression compression(20., 20.);
    const auto own = compression.transmit_fundamental(wave);
    const auto driven = compression.transmit_fundamental(wave, std::norm(wave));
    const auto spectrum =
        rfmodel::MatchedPolynomialAmplifier::from_intercepts("second TU", 20., 20., 10., 75.)
            .transmit({1.e6, {{10, wave}}});
    if (own != driven || !std::isfinite(std::norm(own)) || spectrum.amplitudes.count(0) != 1 ||
        spectrum.amplitudes.count(10) != 1 || spectrum.amplitudes.count(20) != 1 ||
        spectrum.amplitudes.count(30) != 1) {
        throw std::runtime_error("second translation unit SDK check");
    }
}
