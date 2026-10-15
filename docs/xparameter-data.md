# Bounded native X-parameter text data reader

`<rfmodel/io/xparameter_data.hpp>` provides an owning C++17 raw typed-data
reader. It has no RFModel model/device dependencies. It does **not** import a
vendor model, establish numerical compatibility, normalize coefficients, convert
waves, interpolate, fill missing values, or run a simulation. Legacy stored
Volterra normalization remains unresolved; version-2 physical reference planes
and port current polarity remain unestablished. See the
[installed format evidence](systemvue-xparameter-format-evidence-20261015.md).

```cpp
#include <rfmodel/io/xparameter_data.hpp>
#include <sstream>
std::istringstream input("BEGIN PHDModelData\n% AN_1_1(real) FB_2_1(complex)\n"
                         "0.1 0.02 -0.03\nEND\n");
const auto document = rfmodel::read_xparameter_data(input);
const auto &block = document.blocks().at(0);
const auto &value = block.rows.at(0).groups.at(0).values.at(1);
const auto coefficient = std::get<std::complex<double>>(value.value);
```

The filesystem-path overload opens in binary mode and delegates to the stream
reader. Streams are consumed from their current position and are not rewound.
Failures throw `XParameterParseError`, a `std::runtime_error` carrying a 1-based
line/byte-column location; open failures use 1:1. Allocation exceptions propagate.
The document is returned only after complete validation and exposes only const
access to its owned blocks. Copies own independent storage. A caller may freely
copy individual public record types; they are raw records, not certified models.

## Explicitly restricted grammar

- Case-sensitive `BEGIN name` / `END` with no trailing tokens. Supported block
  names are `PHDModelData`, `XParamAttributes`, `XParamPortData`, `XParamData`.
  Repeated data/port blocks are allowed; a second attributes block is rejected.
- `VAR name(type) = value` outside blocks applies only to the immediately next
  block. No earlier VAR is inherited. Dangling variables are rejected.
- `# name(type) = value` inside a block precedes its headers.
- One or more `% name(type) name(type)` lines precede data. Every logical row
  contains exactly one data line per header line, in order, with exact typed
  width. Blank/comment lines do not consume a group. Partial rows are rejected.
- Types are `int`, `real`, `complex`, `string`. Complex uses exactly two finite
  decimal tokens in real/imaginary order. Integers are signed 64-bit decimal
  digits with optional sign; fractional/exponent integer syntax is rejected.
- Reals accept decimal significands with an optional decimal exponent. Hex,
  NaN/Inf, commas, junk, overflow, and nonzero values rounding to zero are
  rejected. Representable subnormals are retained. Conversion uses the classic
  locale and does not require a wider-than-double `long double`.
- Names contain ASCII letters, digits, underscores, preserved verbatim without
  port/bin/DC/mixing interpretation. Field declarations must be separated by
  whitespace. Spaces around declaration punctuation are accepted.
- Strings are a single unquoted token or a single/double quoted token, including
  empty strings and spaces. Backslash escapes, embedded quote concatenation,
  multiline strings, and inline comments are outside this subset and rejected.
  Raw token spelling (including quotes, signs and exponent spelling) is retained
  alongside decoded/typed values. This is not a byte-for-byte file round trip:
  comments and whitespace are not retained.
- Empty lines, whole-line `!` comments and `REM` comments are ignored. `REM` must
  end or be followed by whitespace. LF/CRLF and a UTF-8 BOM at byte zero are
  accepted. CR is whitespace, not a standalone line separator. Columns count
  bytes, including the BOM; unsupported control characters are rejected.
- All variable, constant and header names must be unique within a block's
  combined declaration scope. Duplicate names are allowed in other blocks.
  Unknown declarations, nested blocks, headers/constants after data and missing
  END are errors. Every block must contain at least one header and complete row.

Version is determined from parsed content, never the filename extension or a
comment. Legacy means PHDModelData blocks only. Version 2 requires one
XParamAttributes block with one row containing `Version(real)` exactly 2.0;
its position among blocks is not prescribed. Legacy/version-2 blocks cannot be
mixed. Other metadata fields are retained without interpreting or validating a
complete vendor schema. A raw-data document alone is not a compatibility claim.

## Ownership and resource bounds

`XParameterLimits` defaults bound input bytes (64 MiB), each physical line
(1 MiB excluding LF but including CR), lines (1,000,000), blocks (4096), columns
and header groups per block (16,384 each), rows per block (100,000), total rows
(1,000,000), typed values (4,000,000), token count (8,000,000), declarations per
block (16,384), total declarations (1,000,000), and conservatively charged parsed
text bytes (128 MiB). All can be lowered, including to zero. Value totals include
VAR and constants; a complex is one typed value and two tokens. Declarations
include VAR, constants and header fields.

The parsed-text budget charges names, type-name parse temporaries, duplicate
lookup copies, raw tokens and decoded tokens (even numeric token temporaries).
It is deliberately conservative, not a measurement of final allocation bytes.
Input line storage and fixed object/container overhead have separate bounds
through bytes/line/value/token/declaration limits. These controls do not promise
a precise resident-memory cap; choose tighter limits for hostile inputs. Checks
precede bounded growth, use subtraction to avoid overflow, and check an entire
row's value budget before storing its first physical group. The reader does not
multiply user-controlled dimensions to size an allocation. A failed parse yields
no partial document. Stream I/O errors are distinguished from clean EOF, including
streams configured to throw on EOF.

## Evidence and verification boundary

Synthetic fixtures exercise legacy and version-2 syntax, multiline headers,
RI/string/integer values, physical group ownership, negative mixing labels and
DC labels. The registered evidence values are tested only as decimal text to
stored value. They are neither independent simulator predictions nor validation
of entire vendor files. The repository does not contain those full vendor files;
no complete installed file has been parsed with this C++ reader here.

`scripts/reference/probe-xparameter-data.cpp` is an optional read-only native
probe. Compile with `c++ -std=c++17 -Iinclude` and supply an explicit local file
path. It prints block shapes, stored VAR/constants, raw AN ranges and the first
five first-row FI/FB_2_/S_2_/T_2_ values for comparison with the evidence record.
It does not invoke SystemVue, change any file, perform fitting or run a new
simulation. Running it on installed samples is a separate future verification
step, not something performed by adding this reader.

The [focused validation record](../validation/xparameter-data-validation-20261015.json)
records the checks actually run and the unrun full-vendor/full-repository gates.
