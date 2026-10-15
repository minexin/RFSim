#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rfmodel {
using DcNode = std::size_t;

struct DcChargeEvaluation {
    std::vector<double> charge_coulomb;
    std::vector<double> dcharge_dvoltage_farad; // terminal rows, voltage columns
};

struct DcDeviceEvaluation {
    std::vector<double> current_ampere;            // positive INTO each device terminal
    std::vector<double> dcurrent_dvoltage_siemens; // row-major T by T
    std::optional<DcChargeEvaluation> charge;      // absent: identically zero Q and C
};

struct DcDevice {
    std::string name;
    std::vector<DcNode> terminals; // 0 is ground; tied terminals accumulate
    std::function<DcDeviceEvaluation(const std::vector<double> &)> evaluate;
    // Optional feasibility guard; false trial points backtrack before ANY evaluate call.
    // Predicate exceptions are model errors (fail fast), never feasibility rejections.
    std::function<bool(const std::vector<double> &)> in_domain = {};
};

struct DcNetwork {
    std::size_t non_ground_nodes = 0; // 1..1024, dense solver
    std::vector<DcDevice> devices;
    std::vector<double> injection_ampere; // node n is entry n-1, positive INTO node
};

struct DcSolveOptions {
    std::size_t max_newton_iterations = 100; // accepted updates, final point checked
    std::size_t max_backtracks = 30;         // reductions after the full-step trial
    double armijo = 1e-4;
    double backtrack_factor = 0.5;
    double current_absolute_tolerance_ampere = 1e-12;
    double current_relative_tolerance = 1e-9;
    double voltage_absolute_tolerance_volt = 1e-10;
    double voltage_relative_tolerance = 1e-9;
    std::vector<double> current_scale_ampere; // empty defaults to 1e-3 A/node
    std::vector<double> voltage_scale_volt;   // empty defaults to 1 V/node
};
enum class DcSolveFailure {
    singular_jacobian,
    line_search_failed,
    iteration_limit,
    callback_failed,
    numeric_range,
    domain_violation
};

class DcSolveError : public std::runtime_error {
public:
    DcSolveError(DcSolveFailure failure, const std::string &message)
        : std::runtime_error(message), failure_(failure) {
    }

    DcSolveFailure failure() const noexcept {
        return failure_;
    }

private:
    DcSolveFailure failure_;
};

namespace dc_detail {
inline double finite(double x) {
    if (!std::isfinite(x)) {
        throw DcSolveError(DcSolveFailure::numeric_range, "DC numeric range exceeded");
    }
    return x;
}

// Extended precision is opportunistic, never required for correctness. These
// operand-aware checks also detect loss before narrowing when long double==double.
inline long double product(long double a, long double b) {
    const auto value = a * b;
    if (!std::isfinite(value) ||
        (a != 0 && b != 0 && (value == 0 || std::fpclassify(value) == FP_SUBNORMAL))) {
        throw DcSolveError(DcSolveFailure::numeric_range, "DC product numeric range loss");
    }
    return value;
}

inline long double quotient(long double a, long double b) {
    const auto value = a / b;
    if (!std::isfinite(value) ||
        (a != 0 && (value == 0 || std::fpclassify(value) == FP_SUBNORMAL))) {
        throw DcSolveError(DcSolveFailure::numeric_range, "DC quotient numeric range loss");
    }
    return value;
}

inline double narrow(long double x) {
    if (!std::isfinite(x) || std::abs(x) > std::numeric_limits<double>::max()) {
        throw DcSolveError(DcSolveFailure::numeric_range, "DC numeric range exceeded");
    }
    const auto result = static_cast<double>(x);
    if (x != 0 && (result == 0 || std::fpclassify(result) == FP_SUBNORMAL)) {
        throw DcSolveError(DcSolveFailure::numeric_range, "DC numeric underflow");
    }
    return result;
}

inline void array(const std::vector<double> &x, std::size_t n) {
    if (x.size() != n || !std::all_of(x.begin(), x.end(), [](double v) {
            return std::isfinite(v);
        })) {
        throw std::invalid_argument("invalid DC array shape or nonfinite value");
    }
}

struct Batch {
    std::vector<double> residual, g, c;
    std::vector<DcDeviceEvaluation> devices;
};

inline Batch
evaluate(const DcNetwork &network, const std::vector<double> &v, std::size_t iteration) {
    const auto n = v.size();
    // Check every device domain before executing any current/charge callback.
    for (std::size_t d = 0; d < network.devices.size(); ++d) {
        const auto &device = network.devices[d];
        if (!device.in_domain) {
            continue;
        }
        std::vector<double> vt(device.terminals.size());
        for (std::size_t j = 0; j < vt.size(); ++j) {
            vt[j] = device.terminals[j] ? v[device.terminals[j] - 1] : 0.;
        }
        bool valid = false;
        try {
            valid = device.in_domain(vt);
        } catch (const std::bad_alloc &) {
            throw;
        } catch (...) {
            std::throw_with_nested(DcSolveError(
                DcSolveFailure::callback_failed,
                "DC domain predicate for device " + std::to_string(d) + " (" + device.name +
                    ") at iteration " + std::to_string(iteration) + " threw"));
        }
        if (!valid) {
            throw DcSolveError(DcSolveFailure::domain_violation,
                               "DC device " + std::to_string(d) + " (" + device.name +
                                   ") at iteration " + std::to_string(iteration) +
                                   ": voltage outside explicit model domain");
        }
    }
    Batch b{network.injection_ampere, std::vector<double>(n * n), std::vector<double>(n * n), {}};
    for (auto &x : b.residual) {
        x = -x;
    }
    for (std::size_t d = 0; d < network.devices.size(); ++d) {
        const auto &device = network.devices[d];
        const auto t = device.terminals.size();
        std::vector<double> vt(t);
        for (std::size_t j = 0; j < t; ++j) {
            vt[j] = device.terminals[j] ? v[device.terminals[j] - 1] : 0.;
        }
        DcDeviceEvaluation e;
        try {
            e = device.evaluate(vt);
            array(e.current_ampere, t);
            array(e.dcurrent_dvoltage_siemens, t * t);
            if (e.charge) {
                array(e.charge->charge_coulomb, t);
                array(e.charge->dcharge_dvoltage_farad, t * t);
            }
        } catch (const std::bad_alloc &) {
            throw;
        } catch (...) {
            std::throw_with_nested(DcSolveError(
                DcSolveFailure::callback_failed,
                "DC device " + std::to_string(d) + " (" + device.name + ") at iteration " +
                    std::to_string(iteration) + ": callback failure or invalid returned arrays"));
        }
        try {
            for (std::size_t r = 0; r < t; ++r) {
                if (!device.terminals[r]) {
                    continue;
                }
                const auto row = device.terminals[r] - 1;
                b.residual[row] = finite(b.residual[row] + e.current_ampere[r]);
                for (std::size_t col = 0; col < t; ++col) {
                    if (!device.terminals[col]) {
                        continue;
                    }
                    const auto index = row * n + device.terminals[col] - 1;
                    b.g[index] = finite(b.g[index] + e.dcurrent_dvoltage_siemens[r * t + col]);
                    if (e.charge) {
                        b.c[index] =
                            finite(b.c[index] + e.charge->dcharge_dvoltage_farad[r * t + col]);
                    }
                }
            }
        } catch (const DcSolveError &error) {
            std::throw_with_nested(DcSolveError(
                error.failure(),
                "DC device " + std::to_string(d) + " (" + device.name + ") at iteration " +
                    std::to_string(iteration) + ": " + error.what()));
        }
        b.devices.push_back(std::move(e));
    }
    return b;
}

// Real scaled partial-pivot LU. No fixed siemens floor, inverse, gmin or pseudoinverse.
// All iterates, including zero-residual ones, must pass nonsingularity and backward error.
inline std::vector<double>
correction(const Batch &batch, const std::vector<double> &is, const std::vector<double> &vs) {
    const auto n = is.size();
    std::vector<double> a(n * n), rhs(n);
    double maximum = 0;
    for (std::size_t r = 0; r < n; ++r) {
        rhs[r] = narrow(quotient(-static_cast<long double>(batch.residual[r]), is[r]));
        for (std::size_t c = 0; c < n; ++c) {
            a[r * n + c] = narrow(quotient(product(batch.g[r * n + c], vs[c]), is[r]));
            maximum = std::max(maximum, std::abs(a[r * n + c]));
        }
    }
    if (maximum == 0) {
        throw DcSolveError(DcSolveFailure::singular_jacobian,
                           "DC singular or numerically ill-conditioned Jacobian");
    }
    for (auto &x : a) {
        x = narrow(quotient(x, maximum));
    }
    for (auto &x : rhs) {
        x = narrow(quotient(x, maximum));
    }
    const auto original_a = a, original_rhs = rhs;
    double norm = 0;
    for (std::size_t r = 0; r < n; ++r) {
        double sum = 0;
        for (std::size_t c = 0; c < n; ++c) {
            sum += std::abs(a[r * n + c]);
        }
        norm = std::max(norm, sum);
    }
    const double threshold = 64 * std::numeric_limits<double>::epsilon() * n * norm;
    for (std::size_t k = 0; k < n; ++k) {
        auto pivot = k;
        for (std::size_t r = k + 1; r < n; ++r) {
            if (std::abs(a[r * n + k]) > std::abs(a[pivot * n + k])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot * n + k]) <= threshold) {
            throw DcSolveError(DcSolveFailure::singular_jacobian,
                               "DC singular or numerically ill-conditioned Jacobian");
        }
        for (std::size_t c = 0; c < n; ++c) {
            std::swap(a[k * n + c], a[pivot * n + c]);
        }
        std::swap(rhs[k], rhs[pivot]);
        for (std::size_t r = k + 1; r < n; ++r) {
            const double factor = narrow(quotient(a[r * n + k], a[k * n + k]));
            a[r * n + k] = 0;
            for (std::size_t c = k + 1; c < n; ++c) {
                a[r * n + c] =
                    narrow(static_cast<long double>(a[r * n + c]) - product(factor, a[k * n + c]));
            }
            rhs[r] = narrow(static_cast<long double>(rhs[r]) - product(factor, rhs[k]));
        }
    }
    for (std::size_t r = n; r-- > 0;) {
        long double sum = rhs[r];
        for (std::size_t c = r + 1; c < n; ++c) {
            sum -= product(a[r * n + c], rhs[c]);
        }
        rhs[r] = narrow(quotient(sum, a[r * n + r]));
    }
    const double solution_norm = *std::max_element(rhs.begin(), rhs.end(), [](double x, double y) {
        return std::abs(x) < std::abs(y);
    });
    for (std::size_t r = 0; r < n; ++r) {
        long double residual = -static_cast<long double>(original_rhs[r]), row_norm = 0;
        for (std::size_t c = 0; c < n; ++c) {
            residual += product(original_a[r * n + c], rhs[c]);
            row_norm += std::abs(original_a[r * n + c]);
        }
        const long double denominator =
            std::abs(original_rhs[r]) + product(row_norm, std::abs(solution_norm));
        if (!std::isfinite(residual) || !std::isfinite(denominator) ||
            std::abs(residual) > 256 * std::numeric_limits<double>::epsilon() * n * denominator) {
            throw DcSolveError(DcSolveFailure::numeric_range,
                               "DC Newton backward error exceeds tolerance");
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        rhs[i] = narrow(product(rhs[i], vs[i]));
    }
    return rhs;
}

inline double merit_norm(const Batch &batch, const std::vector<double> &scale) {
    double norm = 0;
    for (std::size_t i = 0; i < scale.size(); ++i) {
        norm = finite(std::hypot(norm, narrow(quotient(batch.residual[i], scale[i]))));
    }
    return norm;
}
} // namespace dc_detail

class DcOperatingPoint {
public:
    const std::vector<double> &voltage_volt() const noexcept {
        return voltage_;
    }

    const std::vector<double> &residual_ampere() const noexcept {
        return batch_.residual;
    }

    const std::vector<double> &conductance_siemens() const noexcept {
        return batch_.g;
    }

    const std::vector<double> &capacitance_farad() const noexcept {
        return batch_.c;
    }

    const std::vector<DcDeviceEvaluation> &device_evaluations() const noexcept {
        return batch_.devices;
    }

    std::size_t iterations() const noexcept {
        return iterations_;
    }

    std::size_t evaluations() const noexcept {
        return evaluations_;
    } // whole network batches

    double current_convergence_norm() const noexcept {
        return current_norm_;
    }

    double voltage_convergence_norm() const noexcept {
        return voltage_norm_;
    }

private:
    std::vector<double> voltage_;
    dc_detail::Batch batch_;
    std::size_t iterations_, evaluations_;
    double current_norm_, voltage_norm_;

    DcOperatingPoint(std::vector<double> v,
                     dc_detail::Batch b,
                     std::size_t iterations,
                     std::size_t evaluations,
                     double current_norm,
                     double voltage_norm)
        : voltage_(std::move(v)), batch_(std::move(b)), iterations_(iterations),
          evaluations_(evaluations), current_norm_(current_norm), voltage_norm_(voltage_norm) {
    }

    friend DcOperatingPoint
    solve_dc(const DcNetwork &, const std::vector<double> &, const DcSolveOptions &);
};

// Synchronous borrowed network: captures must live and remain unchanged throughout solve.
// Callbacks must be deterministic for equal terminal voltages. No callback parallelism;
// caller owns their thread-safety. The same contract applies to in_domain predicates.
// An empty predicate is unconstrained; false initial points fail, false trial points
// backtrack. Predicate exceptions always fail. Domain-only rejections do not count
// as evaluation batches. Returned point owns only numeric accepted-batch values.
// Constant external injections have zero incremental AC drive; source conductances remain.
// This is a finite grounded quasi-static nodal solver, not SPICE/MNA/DAE/transient analysis.
inline DcOperatingPoint solve_dc(const DcNetwork &network,
                                 const std::vector<double> &initial_volt,
                                 const DcSolveOptions &options = {}) {
    const auto n = network.non_ground_nodes;
    if (!n || n > 1024) {
        throw std::invalid_argument("DC node count must be 1..1024");
    }
    dc_detail::array(initial_volt, n);
    dc_detail::array(network.injection_ampere, n);
    for (const auto &device : network.devices) {
        if (device.terminals.empty() || device.terminals.size() > 1024 || !device.evaluate ||
            std::any_of(device.terminals.begin(), device.terminals.end(), [n](DcNode node) {
                return node > n;
            })) {
            throw std::invalid_argument("invalid DC device terminals (1..1024) or callback");
        }
    }
    auto positive = [](double x) {
        return std::isfinite(x) && x > 0;
    };
    auto nonnegative = [](double x) {
        return std::isfinite(x) && x >= 0;
    };
    if (!options.max_newton_iterations || options.max_newton_iterations > 1000000 ||
        options.max_backtracks > 10000 || !(options.armijo > 0 && options.armijo < 1) ||
        !(options.backtrack_factor > 0 && options.backtrack_factor < 1) ||
        !positive(options.current_absolute_tolerance_ampere) ||
        !positive(options.voltage_absolute_tolerance_volt) ||
        !nonnegative(options.current_relative_tolerance) ||
        !nonnegative(options.voltage_relative_tolerance)) {
        throw std::invalid_argument("invalid DC solver options");
    }
    auto is = options.current_scale_ampere, vs = options.voltage_scale_volt;
    if (is.empty()) {
        is.assign(n, 1e-3);
    }
    if (vs.empty()) {
        vs.assign(n, 1.);
    }
    dc_detail::array(is, n);
    dc_detail::array(vs, n);
    std::vector<double> current_tolerance(n);
    for (std::size_t i = 0; i < n; ++i) {
        current_tolerance[i] =
            options.current_absolute_tolerance_ampere + options.current_relative_tolerance * is[i];
        const double voltage_tolerance =
            options.voltage_absolute_tolerance_volt + options.voltage_relative_tolerance * vs[i];
        if (!positive(is[i]) || !positive(vs[i]) || !positive(current_tolerance[i]) ||
            !positive(voltage_tolerance)) {
            throw std::invalid_argument("invalid DC scales or tolerance range");
        }
    }
    auto v = initial_volt;
    auto batch = dc_detail::evaluate(network, v, 0);
    std::size_t evaluations = 1;
    for (std::size_t iteration = 0;; ++iteration) {
        try {
            const auto delta = dc_detail::correction(batch, is, vs);
            double current_norm = 0, voltage_norm = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const auto voltage_tolerance = dc_detail::finite(
                    options.voltage_absolute_tolerance_volt +
                    options.voltage_relative_tolerance * std::max(vs[i], std::abs(v[i])));
                current_norm =
                    std::max(current_norm,
                             dc_detail::finite(std::abs(batch.residual[i]) / current_tolerance[i]));
                voltage_norm = std::max(voltage_norm,
                                        dc_detail::finite(std::abs(delta[i]) / voltage_tolerance));
            }
            if (current_norm <= 1 && voltage_norm <= 1) {
                return DcOperatingPoint(std::move(v),
                                        std::move(batch),
                                        iteration,
                                        evaluations,
                                        current_norm,
                                        voltage_norm);
            }
            if (iteration == options.max_newton_iterations) {
                throw DcSolveError(DcSolveFailure::iteration_limit,
                                   "DC Newton update limit exceeded");
            }
            const double merit = dc_detail::merit_norm(batch, is);
            double alpha = 1;
            bool accepted = false;
            std::string last_domain_rejection;
            for (std::size_t attempt = 0; attempt <= options.max_backtracks; ++attempt) {
                std::vector<double> trial(n);
                bool valid = true, changed = false;
                for (std::size_t i = 0; i < n; ++i) {
                    const long double value =
                        static_cast<long double>(v[i]) + dc_detail::product(alpha, delta[i]);
                    if (!std::isfinite(value) ||
                        std::abs(value) > std::numeric_limits<double>::max()) {
                        valid = false;
                        break;
                    }
                    trial[i] = static_cast<double>(value);
                    changed = changed || trial[i] != v[i];
                }
                if (valid && changed) {
                    dc_detail::Batch candidate;
                    try {
                        candidate = dc_detail::evaluate(network, trial, iteration + 1);
                    } catch (const DcSolveError &e) {
                        if (e.failure() != DcSolveFailure::domain_violation) {
                            throw;
                        }
                        last_domain_rejection = e.what();
                        alpha *= options.backtrack_factor;
                        continue;
                    }
                    ++evaluations;
                    const double trial_merit = dc_detail::merit_norm(candidate, is);
                    const double decrease = 1 - 2 * options.armijo * alpha;
                    // Norm form of squared-residual Armijo avoids overflow of phi.
                    if (decrease >= 0 && trial_merit < merit &&
                        trial_merit <= std::sqrt(decrease) * merit) {
                        v = std::move(trial);
                        batch = std::move(candidate);
                        accepted = true;
                        break;
                    }
                }
                alpha *= options.backtrack_factor;
            }
            if (!accepted) {
                throw DcSolveError(DcSolveFailure::line_search_failed,
                                   "DC Armijo line search exhausted or stagnated" +
                                       (last_domain_rejection.empty()
                                            ? std::string{}
                                            : ": last domain rejection: " + last_domain_rejection));
            }
        } catch (const DcSolveError &error) {
            std::throw_with_nested(
                DcSolveError(error.failure(),
                             "DC iteration " + std::to_string(iteration) + ": " + error.what()));
        }
    }
}
} // namespace rfmodel
