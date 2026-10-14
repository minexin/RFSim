#pragma once
#include "rfmodel/core/device_model.hpp"

namespace rfmodel {
struct TwoPortWave {
    Complex a1{}, a2{}, b1{}, b2{};
};

inline TwoPortWave
evaluate_two_port(const SMatrix &s, Complex source_incident, Complex load_incident = {}) {
    if (s.ports != 2 || s.values.size() != 4) {
        throw std::invalid_argument("two-port matrix required");
    }
    const auto finite = [](Complex value) {
        return std::isfinite(value.real()) && std::isfinite(value.imag());
    };
    for (const auto value : s.values) {
        if (!finite(value)) {
            throw std::invalid_argument("nonfinite S parameter");
        }
    }
    if (!finite(source_incident) || !finite(load_incident)) {
        throw std::invalid_argument("nonfinite incident wave");
    }
    TwoPortWave w;
    w.a1 = source_incident;
    w.a2 = load_incident;
    w.b1 = s(0, 0) * w.a1 + s(0, 1) * w.a2;
    w.b2 = s(1, 0) * w.a1 + s(1, 1) * w.a2;
    if (!finite(w.b1) || !finite(w.b2)) {
        throw std::overflow_error("nonfinite outgoing wave");
    }
    return w;
}
} // namespace rfmodel
