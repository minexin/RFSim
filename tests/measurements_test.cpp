#include "rfmodel/measurements.hpp"
#include "rfmodel/network.hpp"
#include "test_support.hpp"

int main() {
    using namespace rfmodel;
    near(dbm_to_watts(0), 0.001);
    near(watts_to_dbm(1), 30);
    require(std::isinf(watts_to_dbm(0)) && watts_to_dbm(0) < 0, "zero power");
    near(dbm_to_watts(watts_to_dbm(0)), 0);
    near(vswr(0.5), 3);
    near(return_loss_db(0.1), 20);
    require(std::isinf(vswr(1)), "full reflection");
    require(std::isinf(return_loss_db(0)), "matched return loss");
    rejects<std::invalid_argument>([] {
        vswr(1.1);
    });
    rejects<std::invalid_argument>([] {
        watts_to_dbm(-1);
    });
    rejects<std::invalid_argument>([] {
        power_gain_db(1, 0);
    });
    LinearNetwork net;
    net.add(SMatrix{2, {0., 0.5, 0.5, 0.}});
    net.terminate(0, 0., 1.);
    net.terminate(1);
    const auto waves = net.solve();
    const auto input = port_power(waves.incident[0], waves.outgoing[0]);
    const auto output = port_power(waves.incident[1], waves.outgoing[1]);
    near(input.absorbed_w, 1);
    near(output.absorbed_w, -0.25);
    near(input.absorbed_w + output.absorbed_w, 0.75);
    near(power_gain_db(output.outgoing_w, input.incident_w), -6.020599913279624);
}
