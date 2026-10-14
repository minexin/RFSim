#pragma once
#include "rfmodel/analysis/spectrum_analysis.hpp"
#include "rfmodel/coherent/coherent_system.hpp"
#include "rfmodel/conversion/conversion_operating_point.hpp"
#include <optional>

namespace rfmodel {
// Identity records provenance, not a compatibility claim or a factory registry.
struct ModelIdentity {
    std::string type_id;
    std::string revision;
    std::string vendor;
    std::string vendor_version;
};

struct ModelDescriptor {
    ModelIdentity identity;
    std::string name;
    // Dense local physical IDs 0..N-1, fixed power-wave reference coordinates.
    std::vector<PortInfo> ports;
};

struct ModelCapabilities {
    bool linear{}, noise{}, spectrum{}, operating_point{}, coherent{};
};

// Required at conversion sampling: absence of noise data is never silently zero.
enum class ModelNoisePolicy {
    require_model_noise,
    omit_intrinsic_noise
};

struct ModelOperatingPoint {
    double spacing_hz;
    std::vector<ConversionChannel> channels;
    std::vector<Complex> incident;
};

struct ModelFunctions {
    std::function<SMatrix(double)> scattering;
    std::function<NoiseCorrelation(double)> noise;
    // Matched forward transmission, physical ports 0 -> 1. No noise/provenance.
    std::function<PowerWaveSpectrum(const PowerWaveSpectrum &)> spectrum;
    std::function<ConversionLinearization(const ModelOperatingPoint &)> operating_point;
    CoherentSystem::StageOperation coherent;
    std::size_t coherent_inputs{1}, coherent_outputs{1};
};

class ModelCapabilityUnavailable : public std::logic_error {
public:
    explicit ModelCapabilityUnavailable(const std::string &capability)
        : std::logic_error("model capability unavailable: " + capability) {
    }
};

// Immutable callable bundle. Captured values are owned; callers are responsible
// for lifetimes, purity and concurrency of any explicitly captured references.
class ModelAdapter {
    ModelDescriptor descriptor_;
    ModelFunctions functions_;

    static bool finite(Complex value) {
        return std::isfinite(value.real()) && std::isfinite(value.imag());
    }

    static void frequency(double hz) {
        if (!std::isfinite(hz) || hz < 0.) {
            throw std::invalid_argument("invalid model frequency");
        }
    }

    static void require_capability(bool available, const char *name) {
        if (!available) {
            throw ModelCapabilityUnavailable(name);
        }
    }

    void matrix(const SMatrix &value) const {
        if (value.ports != descriptor_.ports.size()) {
            throw std::invalid_argument("model matrix port count differs");
        }
        noise_detail::finite_matrix(value);
    }

public:
    ModelAdapter(ModelDescriptor descriptor, ModelFunctions functions)
        : descriptor_(std::move(descriptor)), functions_(std::move(functions)) {
        const auto &identity = descriptor_.identity;
        if (identity.type_id.empty() || identity.revision.empty() || descriptor_.name.empty() ||
            descriptor_.ports.empty() || descriptor_.ports.size() > 1024) {
            throw std::invalid_argument("invalid model descriptor");
        }
        for (std::size_t i = 0; i < descriptor_.ports.size(); ++i) {
            const auto &port = descriptor_.ports[i];
            if (port.id != i || !finite(port.reference_impedance) ||
                port.reference_impedance.real() <= 0.) {
                throw std::invalid_argument("invalid model port identity/reference");
            }
        }
        if (functions_.noise && !functions_.scattering) {
            throw std::invalid_argument("stationary noise requires a linear model");
        }
        if (functions_.spectrum && (descriptor_.ports.size() != 2 ||
                                    descriptor_.ports[0].reference_impedance.imag() != 0. ||
                                    descriptor_.ports[1].reference_impedance.imag() != 0.)) {
            throw std::invalid_argument("spectrum adapter requires two real port references");
        }
        if (functions_.coherent) {
            if (!functions_.coherent_inputs || functions_.coherent_inputs > 4096 ||
                !functions_.coherent_outputs || functions_.coherent_outputs > 4096) {
                throw std::invalid_argument("invalid coherent model arity");
            }
            for (const auto &port : descriptor_.ports) {
                if (port.reference_impedance.imag() != 0. ||
                    port.reference_impedance != descriptor_.ports[0].reference_impedance) {
                    throw std::invalid_argument(
                        "coherent adapter requires a common real reference");
                }
            }
        }
    }

    const ModelDescriptor &descriptor() const noexcept {
        return descriptor_;
    }

    ModelCapabilities capabilities() const noexcept {
        return {bool(functions_.scattering),
                bool(functions_.noise),
                bool(functions_.spectrum),
                bool(functions_.operating_point),
                bool(functions_.coherent)};
    }

    std::vector<Complex> references() const {
        std::vector<Complex> result;
        for (const auto &port : descriptor_.ports) {
            result.push_back(port.reference_impedance);
        }
        return result;
    }

    SMatrix s_parameters(double hz) const {
        require_capability(bool(functions_.scattering), "linear");
        frequency(hz);
        auto result = functions_.scattering(hz);
        matrix(result);
        return result;
    }

    NoiseCorrelation noise_correlation(double hz) const {
        require_capability(bool(functions_.noise), "noise");
        frequency(hz);
        auto result = functions_.noise(hz);
        matrix(result.watts_per_hz);
        // Validate without replacing the supplied covariance by a reconstruction.
        noise_detail::psd_matrix(result.watts_per_hz);
        return result;
    }

    std::size_t add_to(LinearNetwork &network, double hz) const {
        return network.add_power_waves(s_parameters(hz), references());
    }

    // Sample the declared stationary linear response, not a nonlinear operating point.
    // Channel order is frequency-major, then dense physical port ID. The returned
    // snapshot owns its matrices; source, reflection and source noise start at zero.
    ConversionDevice conversion_device(double spacing_hz,
                                       const std::vector<int> &bins,
                                       ModelNoisePolicy noise_policy) const {
        require_capability(bool(functions_.scattering), "linear");
        if (noise_policy != ModelNoisePolicy::require_model_noise &&
            noise_policy != ModelNoisePolicy::omit_intrinsic_noise) {
            throw std::invalid_argument("invalid model noise policy");
        }
        const bool with_noise = noise_policy == ModelNoisePolicy::require_model_noise;
        if (with_noise) {
            require_capability(bool(functions_.noise), "noise");
        }
        const auto ports = descriptor_.ports.size();
        if (!std::isfinite(spacing_hz) || spacing_hz <= 0. || bins.empty() || bins.size() > 512 ||
            ports > 512 / bins.size()) {
            throw std::invalid_argument("invalid model conversion grid");
        }
        const auto refs = references();
        // Validate the entire grid before invoking any potentially stateful provider.
        for (std::size_t k = 0; k < bins.size(); ++k) {
            if (bins[k] < 0 || (k && bins[k] <= bins[k - 1]) ||
                !std::isfinite(bins[k] * spacing_hz)) {
                throw std::invalid_argument("model conversion bins must increase and be finite");
            }
            if (bins[k] == 0) {
                for (auto ref : refs) {
                    if (ref.imag() != 0.) {
                        throw std::invalid_argument("DC model conversion requires real references");
                    }
                }
            }
        }
        std::vector<SMatrix> samples;
        std::vector<NoiseCorrelation> noise;
        samples.reserve(bins.size());
        noise.reserve(bins.size());
        for (auto bin : bins) {
            const double hz = bin * spacing_hz;
            samples.push_back(s_parameters(hz));
            noise.push_back(with_noise ? noise_correlation(hz)
                                       : NoiseCorrelation{conversion_detail::zero(ports)});
        }
        auto result = lift_linear_conversion(
            spacing_hz, bins, samples, noise, std::vector<std::vector<Complex>>(bins.size(), refs));
        result.model.validate_noise(result.intrinsic_noise);
        return result;
    }

    PowerWaveSpectrum transmit(const PowerWaveSpectrum &incident) const {
        require_capability(bool(functions_.spectrum), "spectrum");
        validate_power_wave_spectrum(incident);
        auto result = functions_.spectrum(incident);
        validate_power_wave_spectrum(result);
        if (result.spacing_hz != incident.spacing_hz) {
            throw std::invalid_argument("model changed spectrum spacing");
        }
        return result;
    }

    SpectrumStage spectrum_stage(std::string name) const {
        require_capability(bool(functions_.spectrum), "spectrum");
        return {std::move(name),
                descriptor_.ports[0].reference_impedance.real(),
                descriptor_.ports[1].reference_impedance.real(),
                [model = *this](const auto &input) {
                    return model.transmit(input);
                }};
    }

    ConversionLinearization linearize(const ModelOperatingPoint &point) const {
        require_capability(bool(functions_.operating_point), "operating_point");
        if (point.channels.empty() || point.channels.size() > 512 ||
            point.incident.size() != point.channels.size()) {
            throw std::invalid_argument("invalid model operating-point dimensions");
        }
        std::vector<Complex> refs;
        for (std::size_t i = 0; i < point.channels.size(); ++i) {
            const auto channel = point.channels[i];
            if (channel.port >= descriptor_.ports.size() || !finite(point.incident[i]) ||
                (channel.bin == 0 && point.incident[i].imag() != 0.)) {
                throw std::invalid_argument("invalid model operating-point port/wave");
            }
            refs.push_back(descriptor_.ports[channel.port].reference_impedance);
        }
        const auto zero = conversion_detail::zero(point.channels.size());
        FrequencyConversionModel validate(point.spacing_hz, point.channels, zero, zero, refs);
        auto result = functions_.operating_point(point);
        if (result.jacobian.spacing_hz() != point.spacing_hz ||
            result.jacobian.channels().size() != point.channels.size() ||
            result.outgoing.size() != point.channels.size() ||
            result.jacobian.reference_impedances_ohms() != refs) {
            throw std::invalid_argument("model Jacobian grid/reference differs");
        }
        for (std::size_t i = 0; i < point.channels.size(); ++i) {
            const auto expected = point.channels[i], actual = result.jacobian.channels()[i];
            if (actual.port != expected.port || actual.bin != expected.bin ||
                !finite(result.outgoing[i]) ||
                (expected.bin == 0 && result.outgoing[i].imag() != 0.)) {
                throw std::invalid_argument("model Jacobian identity/output differs");
            }
        }
        return result;
    }

    ConversionNonlinearDevice operating_device(std::size_t device,
                                               double spacing_hz,
                                               std::vector<ConversionChannel> channels) const {
        require_capability(bool(functions_.operating_point), "operating_point");
        return {device,
                [model = *this, spacing_hz, channels = std::move(channels)](
                    const std::vector<Complex> &incident) {
                    return model.linearize({spacing_hz, channels, incident});
                }};
    }

    std::vector<CoherentSystem::Stream>
    coherent_stage(CoherentSystem &system,
                   std::string name,
                   const std::vector<CoherentSystem::Stream> &inputs,
                   std::vector<std::string> outputs) const {
        require_capability(bool(functions_.coherent), "coherent");
        if (inputs.size() != functions_.coherent_inputs ||
            outputs.size() != functions_.coherent_outputs) {
            throw std::invalid_argument("coherent model arity differs");
        }
        return system.stage(std::move(name),
                            inputs,
                            std::move(outputs),
                            [model = *this](auto &pipeline, const auto &local) {
                                if (pipeline.reference_impedance_ohms() !=
                                    model.descriptor_.ports[0].reference_impedance.real()) {
                                    throw std::invalid_argument("coherent model reference differs");
                                }
                                return model.functions_.coherent(pipeline, local);
                            });
    }
};
} // namespace rfmodel
