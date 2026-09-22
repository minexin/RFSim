#pragma once
#include "device_model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rfmodel {
struct NetworkWaves {
    std::vector<Complex> incident;
    std::vector<Complex> outgoing;
    double relative_residual{};
};

// One frequency point; all blocks use the same positive real reference resistance.
class LinearNetwork {
    struct Block { SMatrix s; std::size_t offset; };
    struct Boundary { std::size_t partner; Complex reflection{}, source{}; bool assigned{}; };
    std::vector<Block> blocks_;
    std::vector<Boundary> boundaries_;
    double reference_;
    static bool finite(Complex x) { return std::isfinite(x.real()) && std::isfinite(x.imag()); }
    void available(std::size_t p) const {
        if (p >= boundaries_.size()) throw std::out_of_range("network port");
        if (boundaries_[p].assigned) throw std::invalid_argument("port already connected or terminated");
    }
public:
    // Selected external ports must be unassigned. Their order defines S rows/columns.
    // All remaining ports must already be connected or terminated without sources.
    SMatrix external_s(const std::vector<std::size_t>& ports) const {
        if (ports.empty()) throw std::invalid_argument("no external ports");
        std::vector<bool> selected(boundaries_.size(),false);
        for (auto p : ports) {
            available(p);
            if (selected[p]) throw std::invalid_argument("duplicate external port");
            selected[p]=true;
        }
        for (std::size_t p=0; p<boundaries_.size(); ++p) {
            if (!selected[p] && !boundaries_[p].assigned)
                throw std::invalid_argument("unassigned internal port");
            if (boundaries_[p].source != Complex{})
                throw std::invalid_argument("S extraction requires zero independent sources");
        }
        SMatrix result{ports.size(),std::vector<Complex>(ports.size()*ports.size())};
        for (std::size_t column=0; column<ports.size(); ++column) {
            auto excitation=*this;
            for (std::size_t i=0; i<ports.size(); ++i)
                excitation.terminate(ports[i],{},i==column ? Complex{1,0} : Complex{});
            const auto waves=excitation.solve();
            for (std::size_t row=0; row<ports.size(); ++row)
                result(row,column)=waves.outgoing[ports[row]];
        }
        return result;
    }
    explicit LinearNetwork(double reference_ohms = 50.0) : reference_(reference_ohms) {
        if (!std::isfinite(reference_) || reference_ <= 0) throw std::invalid_argument("invalid network reference");
    }
    // Returns the first global port index of this block.
    std::size_t add(const SMatrix& s, double reference_ohms = 50.0) {
        if (reference_ohms != reference_) throw std::invalid_argument("reference renormalization required");
        if (!s.ports || s.ports > 1024 || s.values.size() != s.ports*s.ports)
            throw std::invalid_argument("invalid block dimensions");
        for (auto x : s.values) if (!finite(x)) throw std::invalid_argument("nonfinite S parameter");
        const auto offset = boundaries_.size();
        if (offset + s.ports > 1024) throw std::invalid_argument("dense solver port limit exceeded");
        blocks_.push_back({s, offset});
        for (std::size_t i=0; i<s.ports; ++i) boundaries_.push_back({offset+i, {}, {}, false});
        return offset;
    }
    void connect(std::size_t p, std::size_t q) {
        available(p); available(q);
        if (p == q) throw std::invalid_argument("cannot connect port to itself");
        boundaries_[p] = {q, {}, {}, true};
        boundaries_[q] = {p, {}, {}, true};
    }
    // a[p] = source + reflection * b[p]. Matched termination: both zero.
    void terminate(std::size_t p, Complex reflection = {}, Complex source = {}) {
        available(p);
        if (!finite(reflection) || !finite(source)) throw std::invalid_argument("nonfinite boundary");
        boundaries_[p] = {p, reflection, source, true};
    }
    NetworkWaves solve() const {
        const auto n = boundaries_.size();
        if (!n) throw std::invalid_argument("empty network");
        for (const auto& b : boundaries_) if (!b.assigned) throw std::invalid_argument("unterminated port");
        SMatrix s{n, std::vector<Complex>(n*n)};
        for (const auto& block : blocks_)
            for (std::size_t r=0; r<block.s.ports; ++r)
                for (std::size_t c=0; c<block.s.ports; ++c)
                    s(block.offset+r, block.offset+c) = block.s(r,c);
        // b=S*a, a=C*b+e => (I-C*S)*a=e.
        SMatrix matrix{n, std::vector<Complex>(n*n)};
        std::vector<Complex> rhs(n);
        double norm=0;
        for (std::size_t r=0; r<n; ++r) {
            const auto& b = boundaries_[r];
            const Complex multiplier = b.partner == r ? b.reflection : Complex{1,0};
            double row_norm=0;
            for (std::size_t c=0; c<n; ++c) {
                matrix(r,c) = (r==c ? Complex{1,0} : Complex{}) - multiplier*s(b.partner,c);
                if (!finite(matrix(r,c))) throw std::runtime_error("network coefficient overflow");
                row_norm += std::abs(matrix(r,c));
            }
            norm=std::max(norm,row_norm);
            rhs[r]=b.source;
        }
        const auto original=matrix;
        const auto excitation=rhs;
        const double threshold=64*std::numeric_limits<double>::epsilon()*n*norm;
        for (std::size_t k=0; k<n; ++k) {
            std::size_t pivot=k;
            for (std::size_t r=k+1; r<n; ++r)
                if (std::abs(matrix(r,k)) > std::abs(matrix(pivot,k))) pivot=r;
            if (std::abs(matrix(pivot,k)) <= threshold) throw std::runtime_error("singular or ill-conditioned network");
            for (std::size_t c=0; c<n; ++c) std::swap(matrix(k,c),matrix(pivot,c));
            std::swap(rhs[k],rhs[pivot]);
            for (std::size_t r=k+1; r<n; ++r) {
                const Complex factor=matrix(r,k)/matrix(k,k);
                matrix(r,k)=0;
                for (std::size_t c=k+1; c<n; ++c) matrix(r,c)-=factor*matrix(k,c);
                rhs[r]-=factor*rhs[k];
            }
        }
        NetworkWaves result{std::vector<Complex>(n),std::vector<Complex>(n),0};
        for (std::size_t r=n; r-- > 0;) {
            Complex value=rhs[r];
            for (std::size_t c=r+1; c<n; ++c) value-=matrix(r,c)*result.incident[c];
            result.incident[r]=value/matrix(r,r);
            if (!finite(result.incident[r])) throw std::runtime_error("network solution overflow");
        }
        double residual=0, amplitude=0, drive=0;
        for (std::size_t r=0; r<n; ++r) {
            Complex error=-excitation[r];
            for (std::size_t c=0; c<n; ++c) {
                result.outgoing[r]+=s(r,c)*result.incident[c];
                error+=original(r,c)*result.incident[c];
            }
            if (!finite(result.outgoing[r]) || !finite(error)) throw std::runtime_error("network response overflow");
            residual=std::max(residual,std::abs(error));
            amplitude=std::max(amplitude,std::abs(result.incident[r]));
            drive=std::max(drive,std::abs(excitation[r]));
        }
        const double scale=norm*amplitude+drive;
        result.relative_residual=scale>0 ? residual/scale : residual;
        if (!std::isfinite(result.relative_residual) || result.relative_residual>1e-10)
            throw std::runtime_error("network residual exceeds tolerance");
        return result;
    }
};
}
