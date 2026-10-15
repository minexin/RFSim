# Owning bounded linear MNA

`rfmodel/mna/linear_network.hpp` provides a separate `rfmodel::mna` API for
linear resistor/capacitor/inductor networks and independent voltage/current
sources. It does not change `DcNetwork`, its Newton solver, or frozen small-signal
contracts. No nonlinear device, SPICE parser, transient/DAE, noise, S extraction,
frequency sweep, or vendor equivalence is implied.

## Building and results

`NetworkBuilder::add_node()` returns a strong local `NodeId`; `ground` is node 0.
The add methods return an `ElementId` in insertion order. IDs are local indices,
not globally unique capabilities: callers must not mix IDs from unrelated builders.
Every element must join different nodes already created by that builder. R, C,
and L are strictly positive. Use a zero-volt independent source between distinct
nodes for an ideal short, not a zero-valued passive element.

`SourceValue{dc, ac_rms}` specifies independent real DC excitation and complex RMS
AC excitation. A DC value never implicitly becomes an AC source. `finalize()`
copies all data into an owning `Network`; subsequent builder changes or destruction
do not alter it. Solves are const and repeated assembly does not mutate it. Failure
throws `SolveError` without returning a partial result. An empty network succeeds
with only the ground voltage; a created unconnected node is singular.

`solve_dc(network, options)` returns `DcSolution` with real volts, amperes, watts.
`solve_ac(network, frequency_hz, options)` accepts strictly positive frequency and
returns `AcSolution` with complex RMS volts/amperes and complex VA. There is no DC
prerequisite for AC. Index `NodeId.value` addresses `node_voltage` including ground;
index `ElementId.value` addresses `elements`. For each element terminal voltage is
Vp−Vn and current is p→n. Absorbed power is VI for DC and V·conj(I) for AC.
An ideal source normally has negative real power when delivering energy.

## Exact circuit equations

Use exp(+jωt). Unknowns are non-ground node voltages, voltage-source currents,
then inductor currents. If each incidence column is +1 at p and −1 at n, AC is

    [ Y    Bv     Bl  ] [v ]   [ −BI I ]
    [ Bvᵀ   0      0  ] [iV] = [   V   ]
    [ Blᵀ   0    −jωL ] [iL]   [   0   ]

R contributes 1/R and C contributes jωC to the nodal admittance. DC capacitors are
exactly open and inductor rows exactly constrain Vp−Vn=0 while retaining current
unknowns. No large-admittance approximation, gmin, source resistance, redundant
branch removal or least-norm current assignment is used. Floating voltage sources
with a grounded resistive return can solve. Redundant/conflicting source loops,
DC inductor loops and unreferenced voltage components are rejected structurally. An exact connectivity check excludes unreferenced components using
R/V/L branches at DC and R/C/V/L at AC (current sources do not fix voltage);
an exact ideal-constraint graph check rejects V-source cycles and DC V/L cycles.
These checks do not classify source consistency. The subsequent numerical gate
checks other deficiencies; failure does not claim to distinguish inconsistency from redundancy.

## Numerical policy

The solver uses `double` and `std::complex<double>` only, with dense scaled-partial-
pivot elimination. It does not rely on wider `long double`, compute a condition
estimate or certify forward accuracy. A small residual is not a bound on solution
error in an ill-conditioned circuit.

First divide KCL rows by `current_scale_a` and voltage-constraint rows by
`voltage_scale_v`; scale voltage columns by `voltage_scale_v` and current columns
by `current_scale_a`. Defaults are 1 V and 1 mA. Four deterministic row/column
power-of-two equilibration passes follow, each exponent bounded to ±128. Tracked
row/column exponents are reported; recovery multipliers are range checked.
Scaled partial pivoting uses fixed row maxima of the equilibrated matrix,
permuted with the rows, not recomputed from the trailing matrix. Its default
relative pivot rejection threshold is max(pivot_relative_tolerance,
64·epsilon·unknown_count), with a requested default of 1e-14. The fixed
dimension-dependent roundoff floor conservatively rejects roundoff-sized pivots
in larger systems; it is a heuristic guard, not an rcond
estimate or a proof of numerical rank. Callers can tighten but cannot lower this
floor. Diagnostics expose the effective threshold.

The recovered solution is checked first. If a residual check fails, up to three
residual-correction steps reuse the same equilibrated LU and stored row
permutations, rechecking after each correction. This bounded refinement does not
relax acceptance tolerances and is not a convergence guarantee. Its step count is
reported. Every original equation is verified twice:
- row-wise normwise dimensionless backward residual
  |rᵢ|/(|bᵢ|+||Aᵢ||₁·||x||∞) ≤ 1e-12, using the unit-normalized A and x
  before additional equilibration; a zero denominator requires exact zero residual;
- physical residual ≤ absolute tolerance + 1e-10·(|bᵢ|+Σ|Aᵢⱼxⱼ|), using
  1e-12 A absolute for KCL and 1e-9 V for voltage constraints.

This row-wise normwise definition is intentional. A componentwise denominator
Σ|Aᵢⱼxⱼ| rejects any tiny nonzero remnant in a homogeneous constraint vL=0
with ratio one, even for a simple 250 V / 2 µΩ / 3 µH DC circuit (after three
corrections a remnant around 1e-62 V can remain). The row-wise normwise metric
avoids that representational requirement without rounding residuals to zero or
adding an absolute backward-error floor. It permits normwise coefficient
perturbations, including positions whose original coefficient is zero, and thus
protects small components less strongly than componentwise BERR. A large unrelated solution component
cannot bypass the separately enforced physical-unit row residual gate.

All tolerances and physical scales are explicit `SolveOptions` fields. They must
be positive normal values, and relative tolerances less than one. Relaxing a
threshold is a caller policy decision. Diagnostics report maximum backward/KCL/
constraint residuals, minimum fixed-row-scaled pivot and equilibration exponent
range. Existing solvers' thresholds are unchanged.

The conservative supported arithmetic domain is finite normal doubles and exact
zero. Subnormal input values are rejected as `invalid_input`. Nonfinite,
subnormal or detected nonzero-to-zero underflow during assembly, scaling,
elimination, recovery or derived-output computation causes `range_error`.
Products check their real component products before summation, so a mathematically
representable final answer can be rejected when an intermediate is outside this
domain. Complex division uses a checked Smith-style ratio formulation with scalar
real/imaginary-denominator paths. It rejects unsupported intermediate ranges
even where a more elaborate extended-range algorithm might succeed. These
checks are not arbitrary-precision guarantees.
No partial result is returned, including if derived power overflows.

Hard per-network/per-solve budgets are 256 unknowns and 4096 elements; no cumulative
or frequency-sweep allowance exists. Matrix size multiplication is checked before
allocation. `Failure` also distinguishes `budget_exceeded`, `invalid_input`,
`singular_or_ill_conditioned` and `residual_failure`. Allocation exceptions retain
standard C++ semantics. Builder add failures do not append a partially valid element.

## Independent references and verification

- [Qucs MNA](https://qucs.sourceforge.net/tech/node14.html): incidence/source-current blocks.
- [Qucs AC](https://qucs.sourceforge.net/tech/node18.html): complex nodal equations.
- [Qucs constitutive equations](https://qucs.sourceforge.net/tech/node23.html): capacitor/inductor signs.
- [LAPACK ZGESVX](https://netlib.org/lapack/explore-html/d5/dbe/group__gesvx_ga5ba2410b80323221b182ba004c47c62b.html):
  distinction between equilibration, backward error, condition estimation and forward error.
  LAPACK is background, not a runtime dependency or claimed equivalent solver.

Focused tests use hand-written exact circuit answers, check power balance, singular
and range failures, ownership, scaling, ordering and budgets with effective checks
under NDEBUG. `linear_mna_installed` builds a relocated installed-only two-translation-
unit consumer in both full and pure header-only configurations.
