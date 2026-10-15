# Incremental power-wave S from owning linear MNA

`rfmodel/mna/power_wave_s.hpp` adds single-positive-frequency S extraction to the
[owning bounded linear MNA](bounded-linear-mna.md) circuit. It returns the existing
`rfmodel::SMatrix`, using RMS Kurokawa power waves and complex per-port references.
No nonlinear/DC operating point, DC-S, sweep, noise, affine output bias, transient,
C ABI/CLI interface, or SystemVue equivalence is implied.

## API and ownership

```cpp
#include <rfmodel/mna/power_wave_s.hpp>
using namespace rfmodel::mna;
NetworkBuilder builder;
auto left = builder.add_node(), right = builder.add_node();
builder.add_voltage_source(left, right, {}); // exact ideal through
const auto network = builder.finalize();
const auto block = extract_power_wave_s(network, 1.0e9,
    {{left, ground}, {right, ground}}, {{4, 3}, {9, -2}});
// block.scattering(row, column): outgoing row for unit incident column
// block.references_ohm: owning copy in caller's port order
// block.column_diagnostics[column]: independently accepted loaded solve
```

The signature is `extract_power_wave_s(const Network&, double frequency_hz,
const std::vector<Port>&, const std::vector<rfmodel::Complex>& references_ohm,
const SolveOptions& = {})`. `Port` contains `NodeId p, n`. Results own all storage;
no caller input is changed, and no partial result is returned if any column fails.
IDs are builder-local indices, with the same identity limits as the MNA builder.

Ports are oriented terminal pairs, with V=v[p]−v[n] and I entering the DUT at p
and leaving at n. Same-terminal, ground-ground, duplicate, reversed and linearly
dependent port incidence columns are allowed. A same-terminal port has zero
voltage; it does not anchor an otherwise isolated node. Neither ports nor their
reference impedances are silently reordered, deduplicated or defaulted.

Every independent source is deactivated incrementally. Current-source RHS terms
become zero. Voltage-source RHS terms become zero while their exact voltage
constraints and branch-current unknowns remain. Original source values are not
changed. The result is the incremental S in b=S a+b0, without b0; raw driven b/a
is generally different. No DC or bare AC solve is performed first.

## Wave convention and loaded equations

With exp(+jωt), z=R+jX and strictly positive R:

    a = (V + z I)/(2 sqrt(R))
    b = (V − conj(z) I)/(2 sqrt(R))
    Re(V conj(I)) = |a|² − |b|²

For a load's own V=ZL I, S=(ZL−conj(z))/(ZL+z). Open gives +1, short gives
−conj(z)/z, and conjugate match ZL=conj(z) gives zero. Thus an ideal short at
z=4+j3 gives (−7+j24)/25, not −1.

Let A be the deactivated physical MNA matrix, x its internal unknowns, and B the
port incidence embedded in those rows. Port currents Ip are appended last:

    [ A    −B ] [ x  ] = [   0   ]
    [ Bᵀ    Z ] [ Ip ]   [ 2 D a ]

Z=diag(z), D=diag(sqrt(Re z)); Bᵀ is an ordinary transpose. The first rows keep
KCL units in amperes; internal and port boundary rows have voltage units.
Extract one column for each a=e_j, recovering b with checked wave arithmetic.
A single factorization serves all columns; each RHS replays unit normalization
and all four sequential row scalings, followed by independent solve, residual
verification and bounded refinement. Legacy DC/AC share this assembly and LU
kernel, with their original source excitation and arithmetic order unchanged.

## Full loaded state, limits and diagnostics

Assemble the loaded system, then run exact structural gates before numerical
factorization. Reference connectivity includes AC R/C/L/V-source edges and port
edges, but excludes current sources. Every declared node must reach ground.
Only internal ideal voltage sources enter the AC voltage-constraint cycle guard;
port loads, duplicate ports and finite-frequency inductors are not ideal cycles.

A bare open, floating series resistor, or parallel LC resonance may become
solvable after loading. Conversely, a floating differential common-mode gauge,
redundant ideal-source branch currents, isolated declared node, or hidden unloaded
resonance is rejected even if the observable port S could be unique. This first
API requires an accepted unique full loaded state. It does not select a gauge,
remove states, use a pseudoinverse/gmin, or certify rank/conditioning.

At least one port is required. Total augmented unknowns, including every port
current, must be at most 256; the original 4096-element budget still applies.
Matrix dimensions are checked before allocations. Frequency must be positive
finite normal; Re(z) must be positive finite normal and Im(z) finite normal or
exact zero. Options, all checked arithmetic, physical/backward residual gates,
four equilibration passes, and three-step refinement budget follow the existing
MNA policy. A representable mathematical answer may fail because an intermediate
is outside the conservative normal-or-zero arithmetic domain. Internal element
powers are not computed merely to extract S.

`column_diagnostics[j]` reports that RHS's physical KCL/voltage-constraint and
unit-normalized backward residuals and refinement count. Pivot/equilibration data
are shared across columns, including the effective pivot floor
max(requested tolerance, 64·epsilon·augmented_unknown_count). Small residuals and
accepted pivots do not prove forward accuracy. Passivity/reciprocity are test
invariants, not runtime regularization.

`SolveError` reports `invalid_input` for malformed inputs, `budget_exceeded` for
size budgets, `range_error` for unsupported arithmetic, `singular_or_ill_conditioned`
for structural/pivot rejection, or `residual_failure` after bounded refinement.
Allocation failures retain standard C++ behavior.

## Composition and physical loads

Use existing `rfmodel::LinearNetwork::add_power_waves(block.scattering,
block.references_ohm)` and `connect` to join extracted blocks. These impose voltage
continuity and opposing entering currents even with unequal complex references.
A finite-Y helper is not a universal intermediate representation: ideal voltage
constraints such as a short need not have finite Y.

`LinearNetwork::terminate(p, reflection)` instead specifies a=reflection·b in the
DUT's coordinates. A physical external load has V=−ZL I, so its DUT boundary is

    reflection = (ZL − z)/(ZL + conj(z))

This differs from the load's own S=(ZL−conj(z))/(ZL+z). Prefer adding the load as
its own power-wave block and connecting it when its S and references are known.
For z=9−j2 and ZL=5−j2 the DUT boundary is −2/7; through a port referenced to
4+j3, the input S is (5+j4)/41. The tests check both composition paths against an
independently constructed direct series R=5, C=1/2 load at ω=1.

## References and checks

- K. Kurokawa, [Power Waves and the Scattering Matrix](https://doi.org/10.1109/TMTT.1965.1125964),
  IEEE T-MTT 13(2), 194–202 (1965): wave definitions and load-owned reflection.
  The external-load boundary above follows by reversing the load current.
- Ho, Ruehli and Brennan, [The Modified Nodal Approach to Network Analysis](https://doi.org/10.1109/TCS.1975.1084079),
  IEEE TCAS 22(6), 504–509 (1975): branch-current unknowns and exact constraints.
- [LAPACK ZGESVX documentation](https://www.netlib.org/lapack/lapack-3.1.1/html/zgesvx.f.html):
  distinct backward-error, forward-error and condition diagnostics. LAPACK is not
  a dependency or a claim of equivalent numerical guarantees.

`mna_power_wave_s_test` uses independent exact fixtures and physical identities,
checks composition/renormalization, loaded singular cases, range/size failures,
ownership and diagnostics with active checks under NDEBUG. The relocated
`mna_power_wave_s_installed` consumer uses two translation units in both full and
pure header-only SDK installations. Focused checks are separate from final
integration gates and vendor acceptance.
