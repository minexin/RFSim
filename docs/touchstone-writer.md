# Native Touchstone 2.0 stream export

Include `rfmodel/io/touchstone_writer.hpp` and call
`rfmodel::write_touchstone_v2(std::ostream&, const TouchstoneData&)`.
This header-only C++17 API emits single-ended S parameters using **2.0 / Hz / RI / Full**.
It does not sample models, renormalize S, accept paths, own files, or expose JSON/Python interfaces.
The legacy flat forwarding include `rfmodel/touchstone_writer.hpp` is also installed.

```cpp
rfmodel::TouchstoneData data;
data.ports = 1;
data.frequencies_hz = {0.0, 1.0e9};
data.matrices = {{1, {{0.2, 0.1}}}, {1, {{0.3, -0.1}}}};
rfmodel::write_touchstone_v2(output_stream, data);
```

## Data contract

- 1–1024 ports, nonempty frequencies, exactly one full N×N matrix per frequency.
- Frequencies are finite, nonnegative and strictly increasing. DC and its complex S values are preserved.
- Every matrix's port count and vector length must match exactly; all RI components must be finite.
- Option-line R must be positive and finite. Per-port references must either be empty (fallback to R) or contain exactly N positive finite resistances. Output preserves R and always supplies all effective `[Reference]` values.
- Nonempty `noise_samples` is rejected before any output. There is no discard/ignore option.
- `version`, `format`, `frequency_scale`, `matrix_format` and `two_port_order` describe source representation only and never drive export. In-memory frequencies are already Hz and matrices already full complex row-major values. Lower/Upper metadata does not authorize truncating an asymmetric matrix.

Two-port output explicitly declares `[Two-Port Data Order] 12_21` and writes S11, S12, S21, S22 on one line. One-port output has no two-port keyword. Three or more ports use one full matrix row per line, with frequency only on the first row. Output ends with `[End]` and LF. Classic locale and `max_digits10` preserve finite double values, including subnormals, adjacent doubles and signed zeros, without Hz/RI conversion loss.

## Resources and failure behavior

The fixed output payload budget is **64 MiB (67,108,864 bytes)**, including headers, spaces and end marker. All input validation completes first. Serialization uses bounded geometric staging starting at 1 KiB, with no second full-payload copy; old plus new payload allocations during growth total at most 96 MiB, excluding caller-owned input and small stream/reference bookkeeping. Budget arithmetic is bounded before addition; the final byte count is checked against `streamsize` before conversion. Allocation failure or budget failure does not touch the destination.

Input rejection throws `std::invalid_argument`; exceeding the byte budget throws `std::length_error`; allocation exceptions propagate. An initially failed stream (including eofbit), or a null streambuf, throws `std::ios_base::failure` without changing the initial state. After successful staging, the function calls the destination streambuf's `sputn` **once**. Short writes and streambuf exceptions set badbit and throw `std::ios_base::failure` regardless of the caller's exception mask. The underlying streambuf exception is not preserved.

Direct streambuf output intentionally bypasses ostream sentry behavior, so neither tied streams nor unitbuf cause implicit flushes. The writer never flushes, closes, or clears the destination. Caller locale, flags, precision, width, fill, tie and exception mask are unchanged. I/O failure can leave a prefix; successful return means streambuf acceptance, not atomic replacement, successful later flush/close, or durable storage. The caller owns synchronization and final file I/O checks.

## Format authority and verification

The layout is checked against the original [IBIS Touchstone 2.0 specification](https://www.ibis.org/touchstone_ver2.0/touchstone_ver2_0.pdf), printed pages 6–17 (header, reference, full matrix and ordering rules) and 24–25 (noise references and termination). Tests include independently written one-port, nonreciprocal two-port and asymmetric three-port golden byte strings, separate numeric extraction, reader round trips, invalid input, locale/stream-state preservation, partial/throwing writes and real 64 MiB budget overflow. The installed consumer spans two translation units and exercises canonical and forwarding headers in full and pure SDK installations.

This is format-level interoperability work. No live SystemVue import or simulation compatibility is claimed.
