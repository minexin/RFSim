#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rfmodel::mna {

using Complex = std::complex<double>;

// IDs are indices local to a builder and its finalized snapshots. They carry no
// global identity: callers must not mix IDs from unrelated builders.
struct NodeId {
    std::size_t value;

    explicit constexpr NodeId(std::size_t index) : value(index) {
    }
};

struct ElementId {
    std::size_t value;

    explicit constexpr ElementId(std::size_t index) : value(index) {
    }
};

inline constexpr NodeId ground{0};

enum class Failure {
    invalid_input,
    range_error,
    singular_or_ill_conditioned,
    residual_failure,
    budget_exceeded
};

class SolveError : public std::runtime_error {
public:
    SolveError(Failure code, const std::string &message)
        : std::runtime_error(message), code_(code) {
    }

    Failure failure() const noexcept {
        return code_;
    }

private:
    Failure code_;
};

// Volts for a voltage source; amperes for a current source. DC and AC are
// separate excitations; complex AC amplitudes are RMS, never peak.
struct SourceValue {
    double dc = 0;
    Complex ac_rms{};
};

struct SolveOptions {
    double voltage_scale_v = 1;
    double current_scale_a = .001;
    double pivot_relative_tolerance = 1e-14;
    double backward_relative_tolerance = 1e-12;
    double residual_relative_tolerance = 1e-10;
    double kcl_absolute_tolerance_a = 1e-12;
    double constraint_absolute_tolerance_v = 1e-9;
};

struct Diagnostics {
    // Maximum row-wise normwise backward error in unit-normalized coordinates.
    double max_scaled_residual = 0;
    double max_kcl_residual_a = 0;
    double max_constraint_residual_v = 0;
    double min_scaled_pivot = 1;
    double effective_pivot_tolerance = 0;
    std::size_t refinement_steps = 0;
    int min_equilibration_exponent = 0;
    int max_equilibration_exponent = 0;
};

template <class Scalar> struct ElementResult {
    ElementId id;
    Scalar voltage;
    Scalar current;
    // DC: watts. AC: complex VA, RMS V * conjugate(I), absorbed sign.
    Scalar power;
};

template <class Scalar> struct Solution {
    // Index equals NodeId.value, including exact zero at ground.
    std::vector<Scalar> node_voltage;
    // Index equals ElementId.value; current and terminal voltage point p -> n.
    std::vector<ElementResult<Scalar>> elements;
    Diagnostics diagnostics;
};

using DcSolution = Solution<double>;
using AcSolution = Solution<Complex>;

namespace detail {
[[noreturn]] inline void fail(Failure f, const char *message) {
    throw SolveError(f, message);
}

inline bool normal_or_zero(double x) {
    return x == 0 || std::isnormal(x);
}

inline bool valid(Complex x) {
    return normal_or_zero(x.real()) && normal_or_zero(x.imag());
}

inline Complex checked(Complex x) {
    if (!valid(x)) {
        fail(Failure::range_error, "MNA arithmetic outside finite normal-or-zero domain");
    }
    return x;
}

inline double product(double a, double b) {
    const double x = a * b;
    if (!normal_or_zero(x) || (a != 0 && b != 0 && x == 0)) {
        fail(Failure::range_error, "MNA product overflow or underflow");
    }
    return x;
}

inline Complex multiply(Complex a, Complex b) {
    return checked({product(a.real(), b.real()) - product(a.imag(), b.imag()),
                    product(a.real(), b.imag()) + product(a.imag(), b.real())});
}

inline double quotient(double a, double b) {
    const double x = a / b;
    if (!normal_or_zero(x) || (a != 0 && x == 0)) {
        fail(Failure::range_error, "MNA division overflow or underflow");
    }
    return x;
}

inline Complex divide(Complex a, double b) {
    return {quotient(a.real(), b), quotient(a.imag(), b)};
}

inline Complex divide(Complex a, Complex b) {
    // Smith-style division, with checked scalar intermediates. Deliberately
    // reject extreme cases rather than silently drop a component to underflow.
    if (b.imag() == 0) {
        return divide(a, b.real());
    }
    if (b.real() == 0) {
        return {quotient(a.imag(), b.imag()), -quotient(a.real(), b.imag())};
    }
    if (std::abs(b.real()) >= std::abs(b.imag())) {
        const double r = quotient(b.imag(), b.real());
        const double d = checked(Complex{b.real() + product(b.imag(), r), 0}).real();
        return divide(checked({a.real() + product(a.imag(), r), a.imag() - product(a.real(), r)}),
                      d);
    }
    const double r = quotient(b.real(), b.imag());
    const double d = checked(Complex{b.imag() + product(b.real(), r), 0}).real();
    return divide(checked({product(a.real(), r) + a.imag(), product(a.imag(), r) - a.real()}), d);
}

inline double magnitude(Complex x) {
    const double r = std::abs(checked(x));
    if (!normal_or_zero(r)) {
        fail(Failure::range_error, "MNA magnitude outside supported range");
    }
    return r;
}

inline bool positive(double x) {
    return std::isnormal(x) && x > 0;
}
enum class Kind {
    resistor,
    capacitor,
    inductor,
    voltage_source,
    current_source
};

struct Element {
    Kind kind;
    NodeId p, n;
    double value;
    SourceValue source;
};
} // namespace detail

class NetworkBuilder;

class Network {
public:
    std::size_t node_count() const noexcept {
        return nodes_;
    }

    std::size_t element_count() const noexcept {
        return elements_.size();
    }

    const std::vector<detail::Element> &elements() const noexcept {
        return elements_;
    }

private:
    friend class NetworkBuilder;

    Network(std::size_t n, std::vector<detail::Element> e) : nodes_(n), elements_(std::move(e)) {
    }

    std::size_t nodes_;
    std::vector<detail::Element> elements_;
};

class NetworkBuilder {
public:
    NodeId add_node() {
        if (nodes_ >= 257) {
            detail::fail(Failure::budget_exceeded, "MNA node budget exceeded");
        }
        return NodeId{nodes_++};
    }

    ElementId add_resistor(NodeId p, NodeId n, double ohms) {
        return add(detail::Kind::resistor, p, n, ohms, {});
    }

    ElementId add_capacitor(NodeId p, NodeId n, double farads) {
        return add(detail::Kind::capacitor, p, n, farads, {});
    }

    ElementId add_inductor(NodeId p, NodeId n, double henries) {
        return add(detail::Kind::inductor, p, n, henries, {});
    }

    ElementId add_voltage_source(NodeId p, NodeId n, SourceValue source) {
        return add(detail::Kind::voltage_source, p, n, 1, source);
    }

    ElementId add_current_source(NodeId p, NodeId n, SourceValue source) {
        return add(detail::Kind::current_source, p, n, 1, source);
    }

    Network finalize() const {
        if (nodes_ - 1 + branches_ > 256) {
            detail::fail(Failure::budget_exceeded, "MNA unknown budget exceeded");
        }
        return Network{nodes_, elements_};
    }

private:
    ElementId add(detail::Kind kind, NodeId p, NodeId n, double value, SourceValue source) {
        if (p.value >= nodes_ || n.value >= nodes_ || p.value == n.value ||
            !detail::positive(value) || !detail::normal_or_zero(source.dc) ||
            !detail::valid(source.ac_rms)) {
            detail::fail(Failure::invalid_input, "MNA invalid element, node, value or excitation");
        }
        if (elements_.size() >= 4096) {
            detail::fail(Failure::budget_exceeded, "MNA element budget exceeded");
        }
        const ElementId id{elements_.size()};
        elements_.push_back({kind, p, n, value, source});
        if (kind == detail::Kind::inductor || kind == detail::Kind::voltage_source) {
            ++branches_;
        }
        return id;
    }

    std::size_t nodes_ = 1, branches_ = 0;
    std::vector<detail::Element> elements_;
};

namespace detail {
inline void validate_options(const SolveOptions &o) {
    for (double x : {o.voltage_scale_v,
                     o.current_scale_a,
                     o.pivot_relative_tolerance,
                     o.backward_relative_tolerance,
                     o.residual_relative_tolerance,
                     o.kcl_absolute_tolerance_a,
                     o.constraint_absolute_tolerance_v}) {
        if (!positive(x)) {
            fail(Failure::invalid_input, "MNA options must be finite normal and positive");
        }
    }
    if (o.pivot_relative_tolerance >= 1 || o.backward_relative_tolerance >= 1 ||
        o.residual_relative_tolerance >= 1) {
        fail(Failure::invalid_input, "MNA relative tolerances must be less than one");
    }
}

struct LinearResult {
    std::vector<Complex> x;
    Diagnostics diagnostics;
};

inline LinearResult solve_matrix(const std::vector<Complex> &physical,
                                 const std::vector<Complex> &rhs,
                                 std::size_t nv,
                                 const SolveOptions &o) {
    const std::size_t n = rhs.size();
    LinearResult result{{}, {}};
    if (!n) {
        return result;
    }
    std::vector<Complex> a(physical), b(rhs), columns(n);
    for (std::size_t j = 0; j < n; ++j) {
        columns[j] = j < nv ? o.voltage_scale_v : o.current_scale_a;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const double row_unit = i < nv ? o.current_scale_a : o.voltage_scale_v;
        b[i] = divide(b[i], row_unit);
        for (std::size_t j = 0; j < n; ++j) {
            a[i * n + j] = divide(multiply(a[i * n + j], columns[j]), row_unit);
        }
    }
    const auto dimensionless = a;
    const auto dimensionless_rhs = b;
    std::vector<int> re(n, 0), ce(n, 0);
    // Bounded exact powers of two; each pass targets a maximum in [1,2).
    for (int pass = 0; pass < 4; ++pass) {
        for (std::size_t i = 0; i < n; ++i) {
            double maximum = 0;
            for (std::size_t j = 0; j < n; ++j) {
                maximum = std::max(maximum, magnitude(a[i * n + j]));
            }
            if (!maximum) {
                fail(Failure::singular_or_ill_conditioned, "MNA zero row");
            }
            const int e = std::clamp(-std::ilogb(maximum), -128, 128);
            const double scale = std::scalbn(1.0, e);
            for (std::size_t j = 0; j < n; ++j) {
                a[i * n + j] = multiply(a[i * n + j], scale);
            }
            b[i] = multiply(b[i], scale);
            re[i] += e;
        }
        for (std::size_t j = 0; j < n; ++j) {
            double maximum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                maximum = std::max(maximum, magnitude(a[i * n + j]));
            }
            if (!maximum) {
                fail(Failure::singular_or_ill_conditioned, "MNA zero column");
            }
            const int e = std::clamp(-std::ilogb(maximum), -128, 128);
            const double scale = std::scalbn(1.0, e);
            for (std::size_t i = 0; i < n; ++i) {
                a[i * n + j] = multiply(a[i * n + j], scale);
            }
            columns[j] = multiply(columns[j], scale);
            ce[j] += e;
        }
    }
    for (const auto &exponents : {re, ce}) {
        for (int e : exponents) {
            result.diagnostics.min_equilibration_exponent =
                std::min(result.diagnostics.min_equilibration_exponent, e);
            result.diagnostics.max_equilibration_exponent =
                std::max(result.diagnostics.max_equilibration_exponent, e);
        }
    }
    // Conservative dimension-dependent rejection threshold for roundoff-sized
    // pivots. This is not a rank or conditioning guarantee.
    const double pivot_tolerance =
        std::max(o.pivot_relative_tolerance,
                 64 * std::numeric_limits<double>::epsilon() * static_cast<double>(n));
    result.diagnostics.effective_pivot_tolerance = pivot_tolerance;
    const auto equilibrated = a;
    const auto equilibrated_rhs = b;
    std::vector<std::size_t> pivots(n);
    std::vector<double> rowmax(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            rowmax[i] = std::max(rowmax[i], magnitude(a[i * n + j]));
        }
    }
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t pivot = k;
        double best = 0;
        for (std::size_t i = k; i < n; ++i) {
            const double ratio = quotient(magnitude(a[i * n + k]), rowmax[i]);
            if (ratio > best) {
                best = ratio;
                pivot = i;
            }
        }
        if (best <= pivot_tolerance) {
            fail(Failure::singular_or_ill_conditioned, "MNA scaled pivot rejected");
        }
        result.diagnostics.min_scaled_pivot = std::min(result.diagnostics.min_scaled_pivot, best);
        pivots[k] = pivot;
        if (pivot != k) {
            for (std::size_t j = 0; j < n; ++j) {
                std::swap(a[k * n + j], a[pivot * n + j]);
            }
            std::swap(b[k], b[pivot]);
            std::swap(rowmax[k], rowmax[pivot]);
        }
        for (std::size_t i = k + 1; i < n; ++i) {
            const Complex factor = divide(a[i * n + k], a[k * n + k]);
            a[i * n + k] = factor;
            for (std::size_t j = k + 1; j < n; ++j) {
                a[i * n + j] = checked(a[i * n + j] - multiply(factor, a[k * n + j]));
            }
            b[i] = checked(b[i] - multiply(factor, b[k]));
        }
    }
    std::vector<Complex> x(n);
    for (std::size_t end = n; end != 0; --end) {
        const auto i = end - 1;
        Complex value = b[i];
        for (std::size_t j = i + 1; j < n; ++j) {
            value = checked(value - multiply(a[i * n + j], x[j]));
        }
        x[i] = divide(value, a[i * n + i]);
    }
    // Verify the recovered solution in original physical and unit-normalized
    // coordinates, not the further equilibrated factorization coordinates.
    const auto verify = [&]() {
        std::vector<Complex> physical_x(n), unit_x(n);
        double x_norm = 0;
        for (std::size_t j = 0; j < n; ++j) {
            physical_x[j] = multiply(x[j], columns[j]);
            unit_x[j] = divide(physical_x[j], j < nv ? o.voltage_scale_v : o.current_scale_a);
            x_norm = std::max(x_norm, magnitude(unit_x[j]));
        }
        result.diagnostics.max_scaled_residual = 0;
        result.diagnostics.max_kcl_residual_a = 0;
        result.diagnostics.max_constraint_residual_v = 0;
        bool accepted = true;
        for (std::size_t i = 0; i < n; ++i) {
            Complex residual = -rhs[i], scaled = -dimensionless_rhs[i];
            double bound = magnitude(rhs[i]), row_norm = 0;
            for (std::size_t j = 0; j < n; ++j) {
                const auto term = multiply(physical[i * n + j], physical_x[j]);
                residual = checked(residual + term);
                bound += magnitude(term);
                scaled = checked(scaled + multiply(dimensionless[i * n + j], unit_x[j]));
                row_norm += magnitude(dimensionless[i * n + j]);
            }
            const double scaled_bound = magnitude(dimensionless_rhs[i]) + product(row_norm, x_norm);
            if (!normal_or_zero(bound) || !normal_or_zero(row_norm) ||
                !normal_or_zero(scaled_bound)) {
                fail(Failure::range_error, "MNA residual bound overflow");
            }
            const double error = magnitude(residual), scaled_error = magnitude(scaled);
            // No absolute floor in this row-wise normwise backward metric.
            // A zero denominator is admitted only for an exactly zero residual.
            if (scaled_bound == 0 && scaled_error != 0) {
                return false;
            }
            const double backward = scaled_bound == 0 ? 0 : quotient(scaled_error, scaled_bound);
            result.diagnostics.max_scaled_residual =
                std::max(result.diagnostics.max_scaled_residual, backward);
            double &physical_max = i < nv ? result.diagnostics.max_kcl_residual_a
                                          : result.diagnostics.max_constraint_residual_v;
            physical_max = std::max(physical_max, error);
            const double absolute =
                i < nv ? o.kcl_absolute_tolerance_a : o.constraint_absolute_tolerance_v;
            const double limit = absolute + product(o.residual_relative_tolerance, bound);
            if (!normal_or_zero(limit)) {
                fail(Failure::range_error, "MNA residual limit overflow");
            }
            if (backward > o.backward_relative_tolerance || error > limit) {
                accepted = false;
            }
        }
        if (accepted) {
            result.x = std::move(physical_x);
        }
        return accepted;
    };
    if (verify()) {
        return result;
    }
    // Bounded iterative refinement using the same equilibrated LU. In
    // particular, failed residual checks get a bounded correction opportunity.
    for (std::size_t step = 0; step < 3; ++step) {
        std::vector<Complex> correction(equilibrated_rhs);
        bool nonzero = false;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                correction[i] = checked(correction[i] - multiply(equilibrated[i * n + j], x[j]));
            }
            nonzero = nonzero || correction[i] != Complex{};
        }
        if (!nonzero) {
            break;
        }
        for (std::size_t k = 0; k < n; ++k) {
            std::swap(correction[k], correction[pivots[k]]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                correction[i] = checked(correction[i] - multiply(a[i * n + j], correction[j]));
            }
        }
        for (std::size_t end = n; end != 0; --end) {
            const auto i = end - 1;
            for (std::size_t j = i + 1; j < n; ++j) {
                correction[i] = checked(correction[i] - multiply(a[i * n + j], correction[j]));
            }
            correction[i] = divide(correction[i], a[i * n + i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = checked(x[i] + correction[i]);
        }
        ++result.diagnostics.refinement_steps;
        if (verify()) {
            return result;
        }
    }
    fail(Failure::residual_failure, "MNA physical or backward residual rejected");
}

inline void validate_topology(const Network &network, bool ac) {
    const std::size_t count = network.node_count();
    std::vector<std::size_t> references(count), constraints(count);
    for (std::size_t i = 0; i < count; ++i) {
        references[i] = constraints[i] = i;
    }
    const auto root = [](const std::vector<std::size_t> &parents, std::size_t i) {
        while (parents[i] != i) {
            i = parents[i];
        }
        return i;
    };
    for (const auto &e : network.elements()) {
        if (e.kind != Kind::current_source && (ac || e.kind != Kind::capacitor)) {
            const auto p = root(references, e.p.value), n = root(references, e.n.value);
            references[p] = n;
        }
        if (e.kind == Kind::voltage_source || (!ac && e.kind == Kind::inductor)) {
            const auto p = root(constraints, e.p.value), n = root(constraints, e.n.value);
            if (p == n) {
                fail(Failure::singular_or_ill_conditioned, "MNA ideal voltage-constraint cycle");
            }
            constraints[p] = n;
        }
    }
    const auto ground_root = root(references, 0);
    for (std::size_t i = 1; i < count; ++i) {
        if (root(references, i) != ground_root) {
            fail(Failure::singular_or_ill_conditioned, "MNA unreferenced voltage component");
        }
    }
}

inline AcSolution
solve(const Network &network, bool ac, double frequency_hz, const SolveOptions &o) {
    validate_options(o);
    if (ac && !positive(frequency_hz)) {
        fail(Failure::invalid_input, "AC frequency must be finite normal and positive");
    }
    const double omega = ac ? product(6.283185307179586476925286766559, frequency_hz) : 0;
    if (!normal_or_zero(omega)) {
        fail(Failure::range_error, "AC angular frequency overflow");
    }
    validate_topology(network, ac);
    const auto &elements = network.elements();
    const std::size_t nv = network.node_count() - 1;
    std::size_t n = nv;
    std::vector<std::size_t> branch(elements.size(), 0);
    // Fixed unknown ordering: node voltages, voltage-source currents, inductor currents.
    for (auto kind : {Kind::voltage_source, Kind::inductor}) {
        for (std::size_t k = 0; k < elements.size(); ++k) {
            if (elements[k].kind == kind) {
                branch[k] = n++;
            }
        }
    }
    if (n > 256 || elements.size() > 4096 ||
        (n && n > std::numeric_limits<std::size_t>::max() / n)) {
        fail(Failure::budget_exceeded, "MNA assembly budget exceeded");
    }
    std::vector<Complex> a(n * n), b(n);
    const auto stamp = [&](std::size_t i, std::size_t j, Complex value) {
        a[i * n + j] = checked(a[i * n + j] + value);
    };
    for (std::size_t k = 0; k < elements.size(); ++k) {
        const auto &e = elements[k];
        const auto source = ac ? e.source.ac_rms : Complex{e.source.dc, 0};
        if (e.kind == Kind::resistor || e.kind == Kind::capacitor) {
            const Complex y = e.kind == Kind::resistor ? divide(Complex{1, 0}, e.value)
                                                       : Complex{0, product(omega, e.value)};
            checked(y);
            if (e.p.value) {
                stamp(e.p.value - 1, e.p.value - 1, y);
            }
            if (e.n.value) {
                stamp(e.n.value - 1, e.n.value - 1, y);
            }
            if (e.p.value && e.n.value) {
                stamp(e.p.value - 1, e.n.value - 1, -y);
                stamp(e.n.value - 1, e.p.value - 1, -y);
            }
        } else if (e.kind == Kind::current_source) {
            if (e.p.value) {
                b[e.p.value - 1] = checked(b[e.p.value - 1] - source);
            }
            if (e.n.value) {
                b[e.n.value - 1] = checked(b[e.n.value - 1] + source);
            }
        } else {
            const auto j = branch[k];
            if (e.p.value) {
                stamp(e.p.value - 1, j, 1);
                stamp(j, e.p.value - 1, 1);
            }
            if (e.n.value) {
                stamp(e.n.value - 1, j, -1);
                stamp(j, e.n.value - 1, -1);
            }
            if (e.kind == Kind::inductor) {
                stamp(j, j, checked(Complex{0, -product(omega, e.value)}));
            } else {
                b[j] = source;
            }
        }
    }
    auto solved = solve_matrix(a, b, nv, o);
    AcSolution out{{}, {}, solved.diagnostics};
    out.node_voltage.push_back(0);
    for (std::size_t j = 0; j < nv; ++j) {
        out.node_voltage.push_back(solved.x[j]);
    }
    for (std::size_t k = 0; k < elements.size(); ++k) {
        const auto &e = elements[k];
        const auto v = checked(out.node_voltage[e.p.value] - out.node_voltage[e.n.value]);
        Complex i;
        switch (e.kind) {
        case Kind::resistor:
            i = divide(v, e.value);
            break;
        case Kind::capacitor:
            i = multiply(v, Complex{0, product(omega, e.value)});
            break;
        case Kind::current_source:
            i = ac ? e.source.ac_rms : Complex{e.source.dc, 0};
            break;
        default:
            i = solved.x[branch[k]];
            break;
        }
        out.elements.push_back({ElementId{k}, v, i, multiply(v, std::conj(i))});
    }
    return out;
}
} // namespace detail

inline AcSolution
solve_ac(const Network &network, double frequency_hz, const SolveOptions &options = {}) {
    return detail::solve(network, true, frequency_hz, options);
}

inline DcSolution solve_dc(const Network &network, const SolveOptions &options = {}) {
    const auto complex_result = detail::solve(network, false, 0, options);
    DcSolution out{{}, {}, complex_result.diagnostics};
    for (auto v : complex_result.node_voltage) {
        out.node_voltage.push_back(v.real());
    }
    for (const auto &e : complex_result.elements) {
        out.elements.push_back({e.id, e.voltage.real(), e.current.real(), e.power.real()});
    }
    return out;
}
} // namespace rfmodel::mna
