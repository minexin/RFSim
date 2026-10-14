#pragma once
#include "rfmodel/conversion/conversion_network.hpp"
#include <functional>

namespace rfmodel {
struct ConversionLinearization {
    FrequencyConversionModel jacobian;
    std::vector<Complex> outgoing;
};

struct ConversionNonlinearDevice {
    std::size_t device;
    std::function<ConversionLinearization(const std::vector<Complex> &)> evaluate;
};

// Borrowed local accepted-state views, valid only during the synchronous provider
// call. outgoing includes the original fixed offset; fixed_output_offset is that
// original offset, not the derived affine linearization offset. Do not retain
// these references. Provider captures must outlive the call and support any
// concurrent solves made by the caller.
struct ConversionAcceptedState {
    const FrequencyConversionModel &jacobian;
    const std::vector<Complex> &incident;
    const std::vector<Complex> &outgoing;
    const std::vector<Complex> &fixed_output_offset;
};

// Owns both the local coordinate labels and the complete intrinsic C/P block.
struct ConversionIntrinsicNoiseSnapshot {
    double spacing_hz;
    std::vector<ConversionChannel> channels;
    std::vector<Complex> reference_impedances_ohms;
    ConversionNoise noise;
};

struct ConversionIntrinsicNoiseProvider {
    std::size_t device;
    std::function<ConversionIntrinsicNoiseSnapshot(const ConversionAcceptedState &)> evaluate;
};

struct ConversionPortConnection {
    std::size_t first_device, first_port, second_device, second_port;
};

struct ConversionOperatingOptions {
    std::size_t max_iterations = 50, max_backtracks = 24;
    double relative_tolerance = 1e-9, absolute_tolerance = 1e-12;
};

struct ConversionOperatingResult {
    ConversionResult waves;
    std::size_t iterations{}, backtracks{};
    double scaled_residual{};
    std::vector<ConversionDevice> linearized_devices;
    std::vector<Complex> output_offset;
};

namespace conversion_operating_detail {
// Share the iteration and failure contracts, while keeping coordinate admission
// explicit at the public entry points.
inline ConversionOperatingResult
solve(const std::vector<ConversionDevice> &devices,
      const std::vector<ConversionPortConnection> &connections,
      const std::vector<ConversionNonlinearDevice> &nonlinear,
      const std::vector<Complex> &initial,
      const ConversionOperatingOptions &options,
      bool loaded_noise,
      const ConversionNoise *additional_source_noise,
      const std::vector<Complex> &fixed_offset,
      bool power_waves,
      const std::vector<ConversionIntrinsicNoiseProvider> &providers = {}) {
    if (devices.empty() || options.max_iterations == 0 || options.max_iterations > 200 ||
        options.max_backtracks > 40 || !std::isfinite(options.relative_tolerance) ||
        options.relative_tolerance <= 0. || options.relative_tolerance > .01 ||
        !std::isfinite(options.absolute_tolerance) || options.absolute_tolerance <= 0.) {
        throw std::invalid_argument("invalid conversion operating-point options");
    }
    // Reject all registration errors before invoking any deterministic callback.
    // A provider may also be attached to a deterministically linear device.
    std::map<std::size_t, const ConversionIntrinsicNoiseProvider *> noise_callbacks;
    for (const auto &entry : providers) {
        if (entry.device >= devices.size() || !entry.evaluate ||
            !noise_callbacks.emplace(entry.device, &entry).second) {
            throw std::invalid_argument("invalid or repeated intrinsic noise provider");
        }
    }
    const double spacing = devices.front().model.spacing_hz();
    // The network's scalar reference is only a legacy admission policy. In the
    // explicit path every device supplies its own complete local coordinates.
    const double reference = power_waves ? 50. : devices.front().model.reference_ohms();
    std::vector<std::size_t> offsets;
    std::vector<Complex> source, reflection, references;
    std::vector<ConversionChannel> channels;
    FrequencyConversionNetwork topology(spacing, reference);
    for (const auto &device : devices) {
        offsets.push_back(source.size());
        auto validation = device;
        const auto n = device.model.channels().size();
        validation.model = FrequencyConversionModel(spacing,
                                                    device.model.channels(),
                                                    conversion_detail::zero(n),
                                                    conversion_detail::zero(n),
                                                    device.model.reference_impedances_ohms());
        // Validate the original grid/reference too, before replacing the model.
        if (device.model.spacing_hz() != spacing ||
            (!power_waves && device.model.reference_ohms() != reference)) {
            throw std::invalid_argument("operating-point device grid/reference differs");
        }
        if (power_waves) {
            topology.add_power_waves(std::move(validation));
        } else {
            topology.add(std::move(validation));
        }
        source.insert(source.end(), device.source.begin(), device.source.end());
        reflection.insert(reflection.end(), device.reflection.begin(), device.reflection.end());
        const auto &local_references = device.model.reference_impedances_ohms();
        references.insert(references.end(), local_references.begin(), local_references.end());
        channels.insert(
            channels.end(), device.model.channels().begin(), device.model.channels().end());
    }
    const auto total = source.size();
    std::vector<std::size_t> partner(total, total);
    auto boundary_self = reflection;
    std::vector<Complex> boundary_other(total);
    for (const auto &wire : connections) {
        topology.connect(wire.first_device, wire.first_port, wire.second_device, wire.second_port);
        const auto &left = devices[wire.first_device].model.channels();
        const auto &right = devices[wire.second_device].model.channels();
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i].port != wire.first_port) {
                continue;
            }
            for (std::size_t j = 0; j < right.size(); ++j) {
                if (right[j].port == wire.second_port && right[j].bin == left[i].bin) {
                    const auto first = offsets[wire.first_device] + i;
                    const auto second = offsets[wire.second_device] + j;
                    partner[first] = second;
                    partner[second] = first;
                    if (power_waves) {
                        const auto coefficients =
                            power_wave_detail::connection(references[first], references[second]);
                        boundary_self[first] = coefficients.first_reflection;
                        boundary_self[second] = coefficients.second_reflection;
                        boundary_other[first] = boundary_other[second] = coefficients.coupling;
                    } else {
                        boundary_self[first] = boundary_self[second] = 0.;
                        boundary_other[first] = boundary_other[second] = 1.;
                    }
                }
            }
        }
    }
    topology.analyze(false, additional_source_noise, fixed_offset);
    std::map<std::size_t, const ConversionNonlinearDevice *> callbacks;
    for (const auto &entry : nonlinear) {
        if (entry.device >= devices.size() || !entry.evaluate ||
            !callbacks.emplace(entry.device, &entry).second) {
            throw std::invalid_argument("invalid or repeated nonlinear conversion device");
        }
    }
    auto validate_wave = [&](const std::vector<Complex> &wave) {
        if (wave.size() != total) {
            throw std::invalid_argument("operating-point wave dimensions differ");
        }
        for (std::size_t i = 0; i < total; ++i) {
            if (!conversion_detail::finite(wave[i]) ||
                (channels[i].bin == 0 && wave[i].imag() != 0.)) {
                throw std::invalid_argument("invalid operating-point wave or complex DC");
            }
        }
    };
    auto incident = initial.empty() ? source : initial;
    validate_wave(incident);

    struct Evaluation {
        std::vector<ConversionDevice> devices;
        std::vector<Complex> outgoing, offset;
        double residual{}, relative_residual{};
    };

    auto evaluate = [&](const std::vector<Complex> &wave) {
        validate_wave(wave);
        Evaluation e{devices,
                     std::vector<Complex>(total),
                     fixed_offset.empty() ? std::vector<Complex>(total) : fixed_offset,
                     0.};
        for (std::size_t d = 0; d < devices.size(); ++d) {
            const auto first = offsets[d], n = devices[d].model.channels().size();
            const auto found = callbacks.find(d);
            if (found != callbacks.end()) {
                const std::vector<Complex> local(wave.begin() + first, wave.begin() + first + n);
                auto value = found->second->evaluate(local);
                if (value.outgoing.size() != n || value.jacobian.channels().size() != n ||
                    value.jacobian.spacing_hz() != spacing ||
                    value.jacobian.reference_impedances_ohms() !=
                        devices[d].model.reference_impedances_ohms()) {
                    throw std::invalid_argument("nonlinear callback dimensions/grid differ");
                }
                for (std::size_t i = 0; i < n; ++i) {
                    const auto actual = value.jacobian.channels()[i];
                    const auto expected = channels[first + i];
                    if (actual.port != expected.port || actual.bin != expected.bin ||
                        !conversion_detail::finite(value.outgoing[i]) ||
                        (actual.bin == 0 && value.outgoing[i].imag() != 0.)) {
                        throw std::invalid_argument("invalid nonlinear callback channels/output");
                    }
                    e.outgoing[first + i] = value.outgoing[i] + e.offset[first + i];
                    e.offset[first + i] += value.outgoing[i];
                    for (std::size_t j = 0; j < n; ++j) {
                        e.offset[first + i] -=
                            value.jacobian.direct()(i, j) * local[j] +
                            value.jacobian.conjugate()(i, j) * std::conj(local[j]);
                    }
                }
                e.devices[d].model = std::move(value.jacobian);
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    e.outgoing[first + i] = e.offset[first + i];
                    for (std::size_t j = 0; j < n; ++j) {
                        e.outgoing[first + i] +=
                            devices[d].model.direct()(i, j) * wave[first + j] +
                            devices[d].model.conjugate()(i, j) * std::conj(wave[first + j]);
                    }
                }
            }
        }
        for (std::size_t i = 0; i < total; ++i) {
            auto expected = source[i] + boundary_self[i] * e.outgoing[i];
            if (partner[i] != total) {
                expected += boundary_other[i] * e.outgoing[partner[i]];
            }
            if (!conversion_detail::finite(e.outgoing[i]) ||
                !conversion_detail::finite(e.offset[i]) || !conversion_detail::finite(expected)) {
                throw std::overflow_error("nonlinear operating-point evaluation overflow");
            }
            const double scale =
                options.absolute_tolerance +
                options.relative_tolerance * std::max(std::abs(wave[i]), std::abs(expected));
            const double residual = std::abs(wave[i] - expected) / scale;
            if (!std::isfinite(scale) || !std::isfinite(residual)) {
                throw std::overflow_error("nonlinear operating-point residual overflow");
            }
            e.residual = std::max(e.residual, residual);
            const auto magnitude = std::max(std::abs(wave[i]), std::abs(expected));
            if (magnitude > 0.) {
                e.relative_residual =
                    std::max(e.relative_residual, std::abs(wave[i] - expected) / magnitude);
            }
        }
        return e;
    };
    auto make_network = [&](const Evaluation &e) {
        FrequencyConversionNetwork network(spacing, reference);
        for (auto device : e.devices) {
            if (power_waves) {
                network.add_power_waves(std::move(device));
            } else {
                network.add(std::move(device));
            }
        }
        for (const auto &wire : connections) {
            network.connect(
                wire.first_device, wire.first_port, wire.second_device, wire.second_port);
        }
        return network;
    };
    ConversionOperatingResult result;
    auto current = evaluate(incident);
    while (current.residual > 1.) {
        if (result.iterations == options.max_iterations) {
            throw std::runtime_error("conversion operating point exceeded iteration limit");
        }
        const auto candidate = make_network(current).solve_waves(current.offset).incident;
        bool accepted = false;
        double alpha = 1.;
        for (std::size_t backtrack = 0; backtrack <= options.max_backtracks; ++backtrack) {
            std::vector<Complex> trial(total);
            for (std::size_t i = 0; i < total; ++i) {
                trial[i] = (1. - alpha) * incident[i] + alpha * candidate[i];
            }
            try {
                auto next = evaluate(trial);
                if (next.residual <= 1. || next.residual < current.residual * (1. - 1e-4 * alpha)) {
                    incident = std::move(trial);
                    current = std::move(next);
                    result.backtracks += backtrack;
                    accepted = true;
                    break;
                }
            } catch (const std::overflow_error &) {
                // A finite Newton direction can overflow the nonlinear law;
                // reduce the step without treating invalid model contracts as recoverable.
            }
            alpha *= .5;
        }
        if (!accepted) {
            throw std::runtime_error("conversion operating-point line search failed");
        }
        ++result.iterations;
    }
    // Freeze complete intrinsic noise blocks only after convergence, in device
    // index order. This is outside all trial/backtracking exception handling.
    // Exceptions propagate without publishing a partial result; external callback
    // side effects cannot be rolled back, so providers should be pure.
    for (const auto &entry : noise_callbacks) {
        const auto d = entry.first, first = offsets[d];
        const auto &model = current.devices[d].model;
        const auto n = model.channels().size();
        const std::vector<Complex> local_incident(incident.begin() + first,
                                                  incident.begin() + first + n);
        const std::vector<Complex> local_outgoing(current.outgoing.begin() + first,
                                                  current.outgoing.begin() + first + n);
        const std::vector<Complex> local_fixed =
            fixed_offset.empty() ? std::vector<Complex>(n)
                                 : std::vector<Complex>(fixed_offset.begin() + first,
                                                        fixed_offset.begin() + first + n);
        auto snapshot =
            entry.second->evaluate({model, local_incident, local_outgoing, local_fixed});
        if (snapshot.spacing_hz != model.spacing_hz() || snapshot.channels.size() != n ||
            snapshot.reference_impedances_ohms != model.reference_impedances_ohms()) {
            throw std::invalid_argument("intrinsic noise snapshot grid/reference differs");
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (snapshot.channels[i].port != model.channels()[i].port ||
                snapshot.channels[i].bin != model.channels()[i].bin) {
                throw std::invalid_argument("intrinsic noise snapshot channel order differs");
            }
        }
        model.validate_noise(snapshot.noise);
        current.devices[d].intrinsic_noise = std::move(snapshot.noise);
    }
    // Noise uses the accepted point's Jacobian. Return that same point and the
    // exact nonlinear device outputs; boundary mismatch is the reported tolerance,
    // rather than replacing these waves with one extra affine Newton step.
    result.waves =
        make_network(current).analyze(loaded_noise, additional_source_noise, current.offset);
    result.waves.incident = incident;
    result.waves.outgoing = current.outgoing;
    result.waves.relative_residual = current.relative_residual;
    result.scaled_residual = current.residual;
    result.linearized_devices = std::move(current.devices);
    result.output_offset = std::move(current.offset);
    return result;
}
} // namespace conversion_operating_detail

// Damped Newton on a - boundary(F(a)), requiring one common real reference.
// Callbacks evaluate a fixed device law and its exact local derivative.
inline ConversionOperatingResult
solve_conversion_operating_point(const std::vector<ConversionDevice> &devices,
                                 const std::vector<ConversionPortConnection> &connections,
                                 const std::vector<ConversionNonlinearDevice> &nonlinear,
                                 const std::vector<Complex> &initial = {},
                                 const ConversionOperatingOptions &options = {},
                                 bool loaded_noise = false,
                                 const ConversionNoise *additional_source_noise = nullptr,
                                 const std::vector<Complex> &fixed_offset = {}) {
    return conversion_operating_detail::solve(devices,
                                              connections,
                                              nonlinear,
                                              initial,
                                              options,
                                              loaded_noise,
                                              additional_source_noise,
                                              fixed_offset,
                                              false);
}

// Explicit per-channel Kurokawa power-wave coordinates. Physical connections
// preserve V and opposite inward I, including each side's self-reflection.
// Each callback's exact channel order, grid and complete reference vector must
// match its original device; no nonlinear law or boundary is renormalized here.
inline ConversionOperatingResult solve_conversion_operating_point_power_waves(
    const std::vector<ConversionDevice> &devices,
    const std::vector<ConversionPortConnection> &connections,
    const std::vector<ConversionNonlinearDevice> &nonlinear,
    const std::vector<Complex> &initial = {},
    const ConversionOperatingOptions &options = {},
    bool loaded_noise = false,
    const ConversionNoise *additional_source_noise = nullptr,
    const std::vector<Complex> &fixed_offset = {}) {
    return conversion_operating_detail::solve(devices,
                                              connections,
                                              nonlinear,
                                              initial,
                                              options,
                                              loaded_noise,
                                              additional_source_noise,
                                              fixed_offset,
                                              true);
}

// Accepted-state intrinsic noise is computed even when loaded_noise is false:
// that flag suppresses loaded incident/cross statistics, not outgoing noise.
// Existing solver names deliberately keep their original function types.
inline ConversionOperatingResult solve_conversion_operating_point_with_intrinsic_noise(
    const std::vector<ConversionDevice> &devices,
    const std::vector<ConversionPortConnection> &connections,
    const std::vector<ConversionNonlinearDevice> &nonlinear,
    const std::vector<ConversionIntrinsicNoiseProvider> &providers,
    const std::vector<Complex> &initial = {},
    const ConversionOperatingOptions &options = {},
    bool loaded_noise = false,
    const ConversionNoise *additional_source_noise = nullptr,
    const std::vector<Complex> &fixed_offset = {}) {
    return conversion_operating_detail::solve(devices,
                                              connections,
                                              nonlinear,
                                              initial,
                                              options,
                                              loaded_noise,
                                              additional_source_noise,
                                              fixed_offset,
                                              false,
                                              providers);
}

inline ConversionOperatingResult solve_conversion_operating_point_power_waves_with_intrinsic_noise(
    const std::vector<ConversionDevice> &devices,
    const std::vector<ConversionPortConnection> &connections,
    const std::vector<ConversionNonlinearDevice> &nonlinear,
    const std::vector<ConversionIntrinsicNoiseProvider> &providers,
    const std::vector<Complex> &initial = {},
    const ConversionOperatingOptions &options = {},
    bool loaded_noise = false,
    const ConversionNoise *additional_source_noise = nullptr,
    const std::vector<Complex> &fixed_offset = {}) {
    return conversion_operating_detail::solve(devices,
                                              connections,
                                              nonlinear,
                                              initial,
                                              options,
                                              loaded_noise,
                                              additional_source_noise,
                                              fixed_offset,
                                              true,
                                              providers);
}
} // namespace rfmodel
