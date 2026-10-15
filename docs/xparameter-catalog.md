# Restricted version-2 raw catalog and exact selection

`<rfmodel/io/xparameter_catalog.hpp>` adds an owning directory over a parsed
`XParameterDocument`. This is a bounded tool contract for host applications, not
a complete vendor schema, file import certification, or numerical model mapping.
The existing raw reader remains unchanged.

## Ownership and addresses

Construct `XParameterCatalog(read_xparameter_data(input))`. The catalog owns an
immutable document. Copies share immutable storage; `row(i)`, `filter`, and
`select_one` return row handles that also own that storage. A row survives source
stream/document destruction and catalog copy, move, or destruction. References
returned by an accessor are valid while an owning catalog or row remains alive;
retain a row handle, not a reference obtained from a temporary owner. Moved-from
catalogs and rows may be destroyed or reassigned; do not call their accessors.
Empty/default or moved-from input documents are rejected.

`row_count()` and `row(i)` enumerate only XParamData rows, in file order. Each row
reports its zero-based original `block_index()` and `row_index()`. `fields()`
reports the original field, literal family prefix and suffix, block, header group, and column within
that group. `value(name)` returns a const original raw value, including lexical
tokens and source location. An absent field throws, never returns zero.
`raw_document()` retains every parsed metadata/header/VAR/constant/value, including
unknown fields. Comments were not retained by the raw reader.

## Restricted metadata

Version, NumPorts and NumFundFreqs are found by exact header names regardless of
header order/grouping. Version must be real 2.0 (enforced by the raw reader), and
the two counts must be positive int64 values. NumPorts is the highest permitted
port index, not the number of records. Sparse ports are accepted; no N-squared
or fundamental-count-sized arrays are allocated.

Port records are indexed by positive PortNumber, at most NumPorts. Duplicate port
numbers across or within blocks are rejected, never overwritten. RefZ0, when
present, must be complex and is retained unchanged; negative/zero real parts and
nonzero imaginary parts are not rejected under native-model admission rules.
PortName, when present, must be string. Both fields are optional: missing values
are represented by empty optionals. An entirely absent port has no map entry.
No default reference impedance, name, or physical reference plane is supplied.

VAR or # metadata on XParamAttributes or XParamPortData is explicitly unsupported
and rejected; the first contract does not attempt condition-scoped metadata
resolution. Additional named header fields are retained in the raw document.
Data-block # constants remain raw metadata and are not selection conditions.

## Field suffix status

Family means the literal prefix before the first underscore; the entire name is
always preserved. Only AN and FB have a restricted syntactic index parser:
positive decimal port followed by exactly NumFundFreqs decimal mixing integers.
A mixing token `mN` means negative N; zero, leading zeros, and m0 are accepted.
Mixing values fit signed int64, including m9223372036854775808. They are not
converted into frequencies, summed, canonicalized, or merged. This suffix status
does not certify the field's physical type, units, or coefficient semantics.

Invalid syntax, overflow, out-of-range port and mixing-count mismatch produce
explicit field statuses without dropping the raw field or failing the directory.
An invalid result may carry the successfully parsed prefix; only `Parsed` is a
complete index. S, T, XY, XZ, AM, AP, FI and FV are known unsupported families;
no suffix interpretation is attempted. Other families are `UnknownFamily` and
remain losslessly accessible. Missing port metadata remains missing regardless
of a successfully parsed suffix.

## Exact typed selection

A condition is all VAR declarations attached to that XParamData block plus its
first column's value for that row. Each row exposes owning `conditions()` copies.
No VAR is inherited from an earlier block, filled into a heterogeneous block,
or extracted from comments. The raw reader rejects duplicate declaration names,
including a VAR/first-column collision, before catalog construction.

`filter({{"fund_1", 1.0e9}, {"AN_1_1", 0.1}})` is a conjunction on those condition
keys only. Empty filters match all rows. Missing/unknown keys fail to match;
coefficient columns other than the first column cannot serve as conditions.
Unmentioned conditions are unrestricted. Duplicate filter keys throw.

Equality uses the exact typed variant value: int64 1 and double 1.0 are distinct;
finite real or complex components use ordinary C++ equality, so +0 and -0 compare
equal. Strings compare decoded text, not quote style. Nonfinite real/complex
queries are rejected. No tolerance, nearest match, interpolation, or coercion is
performed. Floating-point values are those returned by the raw reader, not an
arbitrary-precision decimal identity.

`filter` returns owning rows and None/One/Multiple status. Duplicate complete
conditions are retained as multiple rows. `select_one` throws on no match or
ambiguity. No "first wins" selection is offered.

## Resource limits and exclusions

`XParameterCatalogLimits` bounds actual XParamData fields and rows, total condition
terms across those rows, actual port records, and actual parsed AN/FB mixing
terms. Limits are checked incrementally before corresponding indexed records are
appended. They do not allocate from declared NumPorts/NumFundFreqs. Original
reader limits still bound input/raw representation; these count limits do not
promise a precise resident-memory bound. Filters return at most row_count rows;
conditions() copies at most the stored row's bounded condition count.

This layer does not create PhdCoefficientSlice or ModelAdapter objects, normalize
waves, enforce native reference impedance conditions, infer a pump, fill absent
coefficients, or claim vendor prediction compatibility. The installed-format
[evidence](systemvue-xparameter-format-evidence-20261015.md) remains the boundary
of vendor observations. Synthetic tests of this utility are not full-file or
SystemVue numerical validation.
