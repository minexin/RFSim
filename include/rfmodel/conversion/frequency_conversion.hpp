#pragma once
#include "rfmodel/core/network_parameters.hpp"
#include "rfmodel/noise/noise_matrix.hpp"
#include "rfmodel/core/power_wave_reference.hpp"
#include <set>
#include <limits>

namespace rfmodel {
struct ConversionChannel {
    std::size_t port{};
    int bin{};
};

// C=E[z*z^H], P=E[z*z^T], both in W/Hz. P is necessary for improper noise.
struct ConversionNoise {
    SMatrix covariance;
    SMatrix complementary;
};

// Deterministic waves only. This result deliberately has no noise quantities.
struct ConversionWaveResult {
    std::vector<Complex> incident, outgoing;
    double relative_residual{};
};

struct ConversionResult {
    std::vector<Complex> incident, outgoing;
    ConversionNoise outgoing_noise;
    double relative_residual{};
    // Populated only when loaded noise is requested. Cross matrices use a first:
    // Cab=E[a*b^H], Pab=E[a*b^T], and need not be Hermitian or symmetric.
    ConversionNoise incident_noise, incident_outgoing_noise;
    std::vector<double> net_noise_into_device_w_per_hz;
};

namespace conversion_detail {
inline SMatrix zero(std::size_t n) {
    return {n, std::vector<Complex>(n * n)};
}

inline bool finite(Complex x) {
    return std::isfinite(x.real()) && std::isfinite(x.imag());
}

inline SMatrix multiply(const SMatrix &a, const SMatrix &b) {
    auto result = zero(a.ports);
    for (std::size_t i = 0; i < a.ports; ++i) {
        for (std::size_t k = 0; k < a.ports; ++k) {
            if (a(i, k) == Complex{}) {
                continue;
            }
            for (std::size_t j = 0; j < a.ports; ++j) {
                result(i, j) += a(i, k) * b(k, j);
            }
        }
    }
    noise_detail::finite_matrix(result);
    return result;
}

inline SMatrix cross_noise(const SMatrix &left, const SMatrix &q, const SMatrix &right) {
    const auto intermediate = multiply(left, q);
    auto result = zero(q.ports);
    for (std::size_t i = 0; i < q.ports; ++i) {
        for (std::size_t j = 0; j < q.ports; ++j) {
            for (std::size_t k = 0; k < q.ports; ++k) {
                result(i, j) += intermediate(i, k) * std::conj(right(j, k));
            }
        }
    }
    noise_detail::finite_matrix(result);
    return result;
}

inline ConversionNoise complex_noise(const SMatrix &q) {
    const auto n = q.ports / 2;
    ConversionNoise result{zero(n), zero(n)};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const double xx = q(2 * i, 2 * j).real(), yy = q(2 * i + 1, 2 * j + 1).real();
            const double xy = q(2 * i, 2 * j + 1).real(), yx = q(2 * i + 1, 2 * j).real();
            result.covariance(i, j) = {xx + yy, yx - xy};
            result.complementary(i, j) = {xx - yy, yx + xy};
        }
    }
    noise_detail::finite_matrix(result.covariance);
    noise_detail::finite_matrix(result.complementary);
    return result;
}
} // namespace conversion_detail

// Fixed-pump, finite-channel model b=A*a+B*conj(a)+c. Channels retain physical
// port and frequency identity. Waves retain their per-channel power-wave references.
class FrequencyConversionModel {
    double spacing_, reference_;
    std::vector<Complex> references_;
    std::vector<ConversionChannel> channels_;
    SMatrix direct_, conjugate_, quadrature_;

    SMatrix quadrature_noise(const ConversionNoise &noise) const {
        const auto n = channels_.size();
        noise_detail::finite_matrix(noise.covariance);
        noise_detail::finite_matrix(noise.complementary);
        if (noise.covariance.ports != n || noise.complementary.ports != n) {
            throw std::invalid_argument("conversion noise dimensions differ");
        }
        auto q = conversion_detail::zero(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                const auto c = noise.covariance(i, j) * .5;
                const auto p = noise.complementary(i, j) * .5;
                q(2 * i, 2 * j) = c.real() + p.real();
                q(2 * i + 1, 2 * j + 1) = c.real() - p.real();
                q(2 * i, 2 * j + 1) = p.imag() - c.imag();
                q(2 * i + 1, 2 * j) = p.imag() + c.imag();
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (channels_[i].bin != 0) {
                continue;
            }
            for (std::size_t j = 0; j < 2 * n; ++j) {
                if (q(2 * i + 1, j) != Complex{} || q(j, 2 * i + 1) != Complex{}) {
                    throw std::invalid_argument("DC noise must be real");
                }
            }
        }
        return noise_detail::psd_matrix(q);
    }

    struct WaveSystem {
        SMatrix equation;
        std::vector<std::size_t> partner;
        std::vector<Complex> boundary_self, boundary_other;
    };

    void validate_wave_inputs(const std::vector<Complex> &source,
                              const std::vector<Complex> &reflection,
                              const std::vector<Complex> &output_offset) const {
        const auto n = channels_.size();
        if (!output_offset.empty()) {
            if (output_offset.size() != n) {
                throw std::invalid_argument("conversion output offset dimensions differ");
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (!conversion_detail::finite(output_offset[i]) ||
                    (channels_[i].bin == 0 && output_offset[i].imag() != 0.)) {
                    throw std::invalid_argument("invalid conversion output offset or complex DC");
                }
            }
        }
        if (source.size() != n || reflection.size() != n) {
            throw std::invalid_argument("conversion boundary dimensions differ");
        }
    }

    WaveSystem wave_system(const std::vector<Complex> &source,
                           const std::vector<Complex> &reflection,
                           const std::vector<std::pair<std::size_t, std::size_t>> &connections,
                           const std::vector<Complex> &output_offset,
                           const ConversionNoise *source_noise = nullptr) const {
        const auto n = channels_.size(), m = 2 * n;
        validate_wave_inputs(source, reflection, output_offset);
        std::vector<std::size_t> partner(n, n);
        auto boundary_self = reflection;
        std::vector<Complex> boundary_other(n);
        for (const auto &connection : connections) {
            const auto a = connection.first, b = connection.second;
            if (a >= n || b >= n || a == b || partner[a] != n || partner[b] != n ||
                channels_[a].bin != channels_[b].bin) {
                throw std::invalid_argument("invalid conversion connection");
            }
            partner[a] = b;
            partner[b] = a;
            const auto wire = power_wave_detail::connection(references_[a], references_[b]);
            boundary_self[a] = wire.first_reflection;
            boundary_self[b] = wire.second_reflection;
            boundary_other[a] = boundary_other[b] = wire.coupling;
            for (auto index : {a, b}) {
                if (source[index] != Complex{} || reflection[index] != Complex{}) {
                    throw std::invalid_argument("connected channel cannot have a boundary drive");
                }
                if (source_noise) {
                    for (std::size_t j = 0; j < n; ++j) {
                        if (source_noise->covariance(index, j) != Complex{} ||
                            source_noise->covariance(j, index) != Complex{} ||
                            source_noise->complementary(index, j) != Complex{} ||
                            source_noise->complementary(j, index) != Complex{}) {
                            throw std::invalid_argument(
                                "connected channel cannot have boundary noise");
                        }
                    }
                }
            }
        }
        auto equation = conversion_detail::zero(m);
        for (std::size_t j = 0; j < n; ++j) {
            if (!conversion_detail::finite(source[j]) ||
                !conversion_detail::finite(reflection[j]) ||
                (channels_[j].bin == 0 && (source[j].imag() != 0. || reflection[j].imag() != 0.))) {
                throw std::invalid_argument("invalid conversion boundary or complex DC");
            }
            const auto subtract = [&](std::size_t destination, Complex coefficient) {
                for (std::size_t i = 0; i < m; ++i) {
                    equation(i, 2 * destination) -= quadrature_(i, 2 * j) * coefficient.real() +
                                                    quadrature_(i, 2 * j + 1) * coefficient.imag();
                    equation(i, 2 * destination + 1) +=
                        quadrature_(i, 2 * j) * coefficient.imag() -
                        quadrature_(i, 2 * j + 1) * coefficient.real();
                }
            };
            subtract(j, boundary_self[j]);
            if (partner[j] != n) {
                subtract(partner[j], boundary_other[j]);
            }
        }
        for (std::size_t i = 0; i < m; ++i) {
            equation(i, i) += 1.;
        }
        noise_detail::finite_matrix(equation);
        return {std::move(equation),
                std::move(partner),
                std::move(boundary_self),
                std::move(boundary_other)};
    }

    template <typename Result>
    void finish_waves(Result &result,
                      const WaveSystem &system,
                      const std::vector<Complex> &source,
                      const std::vector<Complex> &output_offset) const {
        const auto n = channels_.size();
        const auto &partner = system.partner;
        const auto &boundary_self = system.boundary_self;
        const auto &boundary_other = system.boundary_other;
        for (std::size_t i = 0; i < n; ++i) {
            result.incident[i] = boundary_self[i] * result.outgoing[i] + source[i];
            if (partner[i] != n) {
                result.incident[i] += boundary_other[i] * result.outgoing[partner[i]];
            }
            if (!conversion_detail::finite(result.outgoing[i]) ||
                !conversion_detail::finite(result.incident[i])) {
                throw std::overflow_error("conversion waves overflow");
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            Complex residual = -result.outgoing[i];
            double scale = std::abs(result.outgoing[i]);
            if (!output_offset.empty()) {
                residual += output_offset[i];
                scale += std::abs(output_offset[i]);
            }
            for (std::size_t j = 0; j < n; ++j) {
                const auto first = direct_(i, j) * result.incident[j];
                const auto second = conjugate_(i, j) * std::conj(result.incident[j]);
                residual += first + second;
                scale += std::abs(first) + std::abs(second);
            }
            if (!std::isfinite(scale) || !conversion_detail::finite(residual)) {
                throw std::overflow_error("conversion residual overflow");
            }
            if (scale > 0.) {
                result.relative_residual =
                    std::max(result.relative_residual, std::abs(residual) / scale);
            }
        }
        if (result.relative_residual > 512 * n * std::numeric_limits<double>::epsilon()) {
            throw std::domain_error("conversion residual exceeds tolerance");
        }
    }

    template <typename Result>
    void transferred_waves(Result &result,
                           const WaveSystem &system,
                           const SMatrix &inverse,
                           const SMatrix &transfer,
                           const std::vector<Complex> &source,
                           const std::vector<Complex> &output_offset) const {
        const auto n = channels_.size(), m = 2 * n;
        result.incident.resize(n);
        result.outgoing.resize(n);
        std::vector<double> outgoing(m);
        for (std::size_t i = 0; i < m; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                outgoing[i] += transfer(i, 2 * j).real() * source[j].real() +
                               transfer(i, 2 * j + 1).real() * source[j].imag();
                if (!output_offset.empty()) {
                    outgoing[i] += inverse(i, 2 * j).real() * output_offset[j].real() +
                                   inverse(i, 2 * j + 1).real() * output_offset[j].imag();
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            result.outgoing[i] = {outgoing[2 * i], outgoing[2 * i + 1]};
        }
        finish_waves(result, system, source, output_offset);
    }

    // Preserve the full API's order of operations in exceptional numeric ranges,
    // without accepting, validating, or propagating any noise.
    ConversionWaveResult transferred_wave_solve(const WaveSystem &system,
                                                const std::vector<Complex> &source,
                                                const std::vector<Complex> &output_offset) const {
        auto identity = conversion_detail::zero(system.equation.ports);
        for (std::size_t i = 0; i < identity.ports; ++i) {
            identity(i, i) = 1.;
        }
        const auto inverse = parameter_detail::solve(system.equation, identity, 1.);
        const auto transfer = conversion_detail::multiply(inverse, quadrature_);
        ConversionWaveResult result;
        transferred_waves(result, system, inverse, transfer, source, output_offset);
        return result;
    }

public:
    FrequencyConversionModel(double spacing_hz,
                             std::vector<ConversionChannel> channels,
                             SMatrix direct,
                             SMatrix conjugate,
                             double reference_ohms = 50.)
        : FrequencyConversionModel(spacing_hz,
                                   channels,
                                   std::move(direct),
                                   std::move(conjugate),
                                   std::vector<Complex>(channels.size(), reference_ohms)) {
    }

    // Matrices and all boundary quantities are already in these coordinates.
    // DC is a real channel and therefore requires a real reference impedance.
    FrequencyConversionModel(double spacing_hz,
                             std::vector<ConversionChannel> channels,
                             SMatrix direct,
                             SMatrix conjugate,
                             std::vector<Complex> references)
        : spacing_(spacing_hz), reference_(references.empty() ? 0. : references.front().real()),
          references_(std::move(references)), channels_(std::move(channels)),
          direct_(std::move(direct)), conjugate_(std::move(conjugate)) {
        const auto n = channels_.size();
        if (!n || n > 512 || !std::isfinite(spacing_) || spacing_ <= 0. ||
            !std::isfinite(reference_) || reference_ <= 0.) {
            throw std::invalid_argument("invalid conversion channels or reference");
        }
        power_wave_detail::references(references_, n);
        for (std::size_t i = 0; i < n; ++i) {
            if (channels_[i].bin == 0 && references_[i].imag() != 0.) {
                throw std::invalid_argument("DC conversion reference must be real");
            }
        }
        noise_detail::finite_matrix(direct_);
        noise_detail::finite_matrix(conjugate_);
        if (direct_.ports != n || conjugate_.ports != n) {
            throw std::invalid_argument("conversion matrix dimensions differ");
        }
        std::set<std::pair<std::size_t, int>> seen;
        for (const auto &channel : channels_) {
            if (channel.port >= 1024 || channel.bin < 0 || !std::isfinite(channel.bin * spacing_) ||
                !seen.emplace(channel.port, channel.bin).second) {
                throw std::invalid_argument("invalid or duplicate conversion channel");
            }
        }
        quadrature_ = conversion_detail::zero(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                const auto sum = direct_(i, j) + conjugate_(i, j);
                const auto difference = direct_(i, j) - conjugate_(i, j);
                quadrature_(2 * i, 2 * j) = sum.real();
                quadrature_(2 * i + 1, 2 * j) = sum.imag();
                if (channels_[j].bin != 0) {
                    quadrature_(2 * i, 2 * j + 1) = -difference.imag();
                    quadrature_(2 * i + 1, 2 * j + 1) = difference.real();
                }
            }
        }
        noise_detail::finite_matrix(quadrature_);
        for (std::size_t i = 0; i < n; ++i) {
            if (channels_[i].bin != 0) {
                continue;
            }
            for (std::size_t j = 0; j < 2 * n; ++j) {
                if (quadrature_(2 * i + 1, j) != Complex{}) {
                    throw std::invalid_argument("conversion must produce real DC waves");
                }
            }
        }
    }

    double spacing_hz() const {
        return spacing_;
    }

    double reference_ohms() const {
        for (const auto value : references_) {
            if (value != Complex{reference_, 0.}) {
                throw std::invalid_argument("conversion model has no common real reference");
            }
        }
        return reference_;
    }

    const std::vector<Complex> &reference_impedances_ohms() const {
        return references_;
    }

    const SMatrix &direct() const {
        return direct_;
    }

    const SMatrix &conjugate() const {
        return conjugate_;
    }

    const std::vector<ConversionChannel> &channels() const {
        return channels_;
    }

    ConversionNoise zero_noise() const {
        return {conversion_detail::zero(channels_.size()),
                conversion_detail::zero(channels_.size())};
    }

    // Validate a standalone C/P source before it is combined with independent noise.
    void validate_noise(const ConversionNoise &noise) const {
        quadrature_noise(noise);
    }

    // Pure deterministic entry point: no C/P is accepted, validated, or propagated.
    // A boundary or intrinsic-noise validity check requires analyze/validate_noise.
    // Normal-range equations use one vector RHS. Detected overflow/underflow risk
    // uses the full deterministic transfer order, without noise, to preserve range.
    ConversionWaveResult
    solve_waves(const std::vector<Complex> &source,
                const std::vector<Complex> &reflection,
                const std::vector<std::pair<std::size_t, std::size_t>> &connections = {},
                const std::vector<Complex> &output_offset = {}) const {
        const auto n = channels_.size(), m = 2 * n;
        const auto system = wave_system(source, reflection, connections, output_offset);
        std::vector<Complex> rhs(m);
        bool range_loss = false;
        const auto unsafe = [](double value) {
            return !std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL;
        };
        for (std::size_t i = 0; i < m; ++i) {
            double sum = 0.;
            const auto add_product = [&](double coefficient, double value) {
                const double product = coefficient * value;
                range_loss = range_loss || unsafe(product) ||
                             (coefficient != 0. && value != 0. && product == 0.);
                sum += product;
                range_loss = range_loss || unsafe(sum);
            };
            for (std::size_t j = 0; j < n; ++j) {
                add_product(quadrature_(i, 2 * j).real(), source[j].real());
                add_product(quadrature_(i, 2 * j + 1).real(), source[j].imag());
            }
            if (!output_offset.empty()) {
                sum += i % 2 == 0 ? output_offset[i / 2].real() : output_offset[i / 2].imag();
                range_loss = range_loss || unsafe(sum);
            }
            rhs[i] = sum;
        }
        if (range_loss) {
            return transferred_wave_solve(system, source, output_offset);
        }
        std::vector<Complex> outgoing;
        try {
            outgoing = parameter_detail::solve(system.equation, rhs, 1.);
        } catch (const std::overflow_error &) {
            return transferred_wave_solve(system, source, output_offset);
        }
        for (std::size_t i = 0; i < m; ++i) {
            if (unsafe(outgoing[i].real()) || (outgoing[i] == Complex{} && rhs[i] != Complex{})) {
                return transferred_wave_solve(system, source, output_offset);
            }
        }
        ConversionWaveResult result;
        result.incident.resize(n);
        result.outgoing.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            result.outgoing[i] = {outgoing[2 * i].real(), outgoing[2 * i + 1].real()};
        }
        finish_waves(result, system, source, output_offset);
        return result;
    }

    // a=Gamma*b+source. Source and intrinsic noises are independent; correlations
    // within either set, including across frequencies, are retained in C and P.
    ConversionResult
    analyze(const std::vector<Complex> &source,
            const std::vector<Complex> &reflection,
            const ConversionNoise &source_noise,
            const ConversionNoise &intrinsic_noise,
            const std::vector<std::pair<std::size_t, std::size_t>> &connections = {},
            bool loaded_noise = false,
            const std::vector<Complex> &output_offset = {}) const {
        const auto n = channels_.size(), m = 2 * n;
        validate_wave_inputs(source, reflection, output_offset);
        // The full API always validates and propagates C/P, even if loaded_noise is false.
        auto source_q = quadrature_noise(source_noise);
        auto intrinsic_q = quadrature_noise(intrinsic_noise);
        const auto system =
            wave_system(source, reflection, connections, output_offset, &source_noise);
        const auto &equation = system.equation;
        const auto &partner = system.partner;
        const auto &boundary_self = system.boundary_self;
        const auto &boundary_other = system.boundary_other;
        auto identity = conversion_detail::zero(m);
        for (std::size_t i = 0; i < m; ++i) {
            identity(i, i) = 1.;
        }
        const auto inverse = parameter_detail::solve(equation, identity, 1.);
        const auto transfer = conversion_detail::multiply(inverse, quadrature_);
        ConversionResult result;
        transferred_waves(result, system, inverse, transfer, source, output_offset);
        auto total = propagate_noise(transfer, {source_q}).watts_per_hz;
        const auto intrinsic = propagate_noise(inverse, {intrinsic_q}).watts_per_hz;
        for (std::size_t i = 0; i < total.values.size(); ++i) {
            total.values[i] += intrinsic.values[i];
        }
        result.outgoing_noise = conversion_detail::complex_noise(total);
        if (loaded_noise) {
            auto boundary = conversion_detail::zero(m);
            for (std::size_t i = 0; i < n; ++i) {
                const auto assign = [&](std::size_t j, Complex coefficient) {
                    boundary(2 * i, 2 * j) = coefficient.real();
                    boundary(2 * i, 2 * j + 1) = -coefficient.imag();
                    boundary(2 * i + 1, 2 * j) = coefficient.imag();
                    boundary(2 * i + 1, 2 * j + 1) = coefficient.real();
                };
                assign(i, boundary_self[i]);
                if (partner[i] != n) {
                    assign(partner[i], boundary_other[i]);
                }
            }
            const auto incident_intrinsic = conversion_detail::multiply(boundary, inverse);
            auto incident_source = conversion_detail::multiply(boundary, transfer);
            for (std::size_t i = 0; i < m; ++i) {
                incident_source(i, i) += 1.;
            }
            auto incident_q = propagate_noise(incident_source, {source_q}).watts_per_hz;
            const auto incident_c = propagate_noise(incident_intrinsic, {intrinsic_q}).watts_per_hz;
            auto cross_q = conversion_detail::cross_noise(incident_source, source_q, transfer);
            const auto cross_c =
                conversion_detail::cross_noise(incident_intrinsic, intrinsic_q, inverse);
            for (std::size_t i = 0; i < incident_q.values.size(); ++i) {
                incident_q.values[i] += incident_c.values[i];
                cross_q.values[i] += cross_c.values[i];
            }
            result.incident_noise = conversion_detail::complex_noise(incident_q);
            result.incident_outgoing_noise = conversion_detail::complex_noise(cross_q);
            result.net_noise_into_device_w_per_hz.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double net = result.incident_noise.covariance(i, i).real() -
                                   result.outgoing_noise.covariance(i, i).real();
                if (!std::isfinite(net)) {
                    throw std::overflow_error("conversion net noise overflow");
                }
                result.net_noise_into_device_w_per_hz[i] = net;
            }
        }
        return result;
    }
};

// Existing real-mixer convention: y=2*g*x*cos(LO*t+phase). Every generated IF
// channel must be declared; no implicit image rejection or frequency truncation.
inline FrequencyConversionModel
ideal_mixer_conversion(double spacing_hz,
                       const std::vector<ConversionChannel> &channels,
                       int lo_bin,
                       double gain_db,
                       double phase_radians = 0.,
                       std::size_t rf_port = 0,
                       std::size_t if_port = 1,
                       double reference_ohms = 50.) {
    if (channels.empty() || channels.size() > 512 || lo_bin <= 0 || rf_port == if_port ||
        !std::isfinite(gain_db) || !std::isfinite(phase_radians) ||
        !std::isfinite(lo_bin * spacing_hz)) {
        throw std::invalid_argument("invalid conversion mixer parameters");
    }
    const double gain = std::pow(10., gain_db / 20.);
    if (!std::isfinite(gain) || gain == 0.) {
        throw std::overflow_error("conversion mixer gain range");
    }
    const auto pump = std::polar(gain, phase_radians);
    auto a = conversion_detail::zero(channels.size()), b = a;
    for (const auto &channel : channels) {
        if (channel.port != rf_port && channel.port != if_port) {
            throw std::invalid_argument("mixer channels must belong to RF or IF port");
        }
    }
    bool has_rf = false;
    auto add = [&](long long bin, std::size_t input, Complex direct, Complex conjugate) {
        if (bin < 0 || bin > std::numeric_limits<int>::max()) {
            throw std::overflow_error("conversion mixer bin range");
        }
        for (std::size_t row = 0; row < channels.size(); ++row) {
            if (channels[row].port == if_port && channels[row].bin == bin) {
                a(row, input) += direct;
                b(row, input) += conjugate;
                return;
            }
        }
        throw std::invalid_argument("missing generated mixer output channel");
    };
    for (std::size_t column = 0; column < channels.size(); ++column) {
        if (channels[column].port != rf_port) {
            continue;
        }
        has_rf = true;
        const auto bin = channels[column].bin;
        if (bin == 0) {
            add(lo_bin, column, std::sqrt(2.) * pump, {});
            continue;
        }
        add(static_cast<long long>(bin) + lo_bin, column, pump, {});
        const auto difference = static_cast<long long>(bin) - lo_bin;
        if (difference > 0) {
            add(difference, column, std::conj(pump), {});
        } else if (difference < 0) {
            add(-difference, column, {}, pump);
        } else {
            add(0, column, std::conj(pump) / std::sqrt(2.), pump / std::sqrt(2.));
        }
    }
    if (!has_rf) {
        throw std::invalid_argument("conversion mixer has no RF channel");
    }
    return FrequencyConversionModel(
        spacing_hz, channels, std::move(a), std::move(b), reference_ohms);
}
} // namespace rfmodel
