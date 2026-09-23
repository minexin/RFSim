#include "rfmodel/ideal_mixer.hpp"
#include "test_support.hpp"
#include <limits>

int main() {
    using namespace rfmodel;
    const double half_pi = 3.14159265358979323846 / 2.;
    IdealRealMixer mixer("mixer", 2, 0., half_pi);
    const auto output = mixer.transmit({1e6, {{10, 1.}}});
    near(output.amplitudes.at(12), Complex{0., 1.});
    near(output.amplitudes.at(8), Complex{0., -1.});
    // RF below LO: negative difference frequency must be conjugated.
    near(mixer.transmit({1., {{1, Complex{0., 1.}}}}).amplitudes.at(1), 1.);
    IdealRealMixer zero_phase("zero", 2, 0.);
    const auto homodyne = zero_phase.transmit({1., {{2, 1.}}});
    near(homodyne.amplitudes.at(0), std::sqrt(2.));
    near(homodyne.amplitudes.at(4), 1.);
    near(zero_phase.transmit({1., {{0, 1.}}}).amplitudes.at(2), std::sqrt(2.));
    // Two distinct RF tones can land on the same IF and interfere coherently.
    near(zero_phase.transmit({1., {{1, 1.}, {3, -1.}}}).amplitudes.count(1), 0.);
    IdealRealMixer loss("loss", 2, -6.020599913279624, 0., 75.);
    near(std::norm(loss.transmit({1., {{10, 1.}}}).amplitudes.at(8)), .25);
    near(loss.port(0).reference_impedance, 75.);
    rejects<std::invalid_argument>([] {
        IdealRealMixer bad("bad", 0, 0.);
    });
    rejects<std::invalid_argument>([&] {
        mixer.transmit({1., {{0, Complex{0., 1.}}}});
    });
    rejects<std::overflow_error>([&] {
        mixer.transmit({1., {{std::numeric_limits<int>::max(), 1.}}});
    });
    rejects<std::overflow_error>([&] {
        IdealRealMixer("overflow", 2, 300., half_pi).transmit({1., {{2, 1e300}}});
    });
}
