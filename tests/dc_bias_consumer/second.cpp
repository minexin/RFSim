#include <rfmodel/dc_operating_point.hpp>
#include <rfmodel/frozen_small_signal.hpp>

rfmodel::Complex second() {
    return rfmodel::admittance_to_power_wave_s({1, {{0, 0}}}, {{50, 20}})(0, 0);
}
