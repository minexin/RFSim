#pragma once
#include <rfmodel/adapters/model_adapter.hpp>
#include <memory>

namespace frozen_example {
enum class Noise {
    missing,
    explicit_zero,
    supplied
};

struct Diagnostics {
    int dc_evaluations{};
    std::weak_ptr<const int> dc_callback_lifetime;
};

// Returns only owned frozen numeric state, port coordinates and optional explicit Q.
rfmodel::ModelAdapter make_model(Diagnostics &diagnostics, Noise noise);
void verify_workflows();
} // namespace frozen_example
