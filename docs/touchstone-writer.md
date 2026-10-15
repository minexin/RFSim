# Native Touchstone 2.0 stream export

Include `rfmodel/io/touchstone_writer.hpp` and call
`rfmodel::write_touchstone_v2(std::ostream&, const TouchstoneData&)`.
This header-only C++17 API emits single-ended S parameters using **2.0 / Hz / RI / Full**, with optional raw two-port noise records.
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
- Nonempty `noise_samples` is supported only for two ports, under the raw noise contract below. Empty noise preserves the previous S-only output byte for byte; no zero count or empty noise block is emitted. There is no discard/ignore option.
- `version`, `format`, `frequency_scale`, `matrix_format` and `two_port_order` describe source representation only and never drive export. In-memory frequencies are already Hz and matrices already full complex row-major values. Lower/Upper metadata does not authorize truncating an asymmetric matrix.

Two-port output explicitly declares `[Two-Port Data Order] 12_21` and writes S11, S12, S21, S22 on one line. One-port output has no two-port keyword. Three or more ports use one full matrix row per line, with frequency only on the first row. Output ends with `[End]` and LF. Classic locale and `max_digits10` preserve finite double values, including subnormals, adjacent doubles and signed zeros, without Hz/RI conversion loss.

### Raw two-port noise

The writer serializes the existing `TouchstoneNoiseSample` values without sampling, fitting, clamping, reconstructing covariance, or checking model-level physical realizability:

- Noise frequencies are finite, nonnegative and strictly increasing in their original order. Their grid is independent of the S grid. The first noise frequency must not exceed the last S frequency; later noise frequencies may exceed it. There is no sorting, interpolation or resampling.
- `minimum_noise_figure_db` is any finite dB value. `noise_resistance_ohms` is finite and nonnegative, and is emitted directly in physical ohms. In particular, data read from a legacy file has already had its normalized resistance converted to ohms; `version=Legacy` does not cause another multiplication or division by R.
- Both components of `optimum_source_reflection` must be finite. Its magnitude is computed using `hypot`, avoiding intermediate squaring overflow/underflow; a nonfinite magnitude or a nonzero input converted to zero magnitude is rejected. Finite subnormal magnitudes are accepted. No `Fmin >= 0` or `|GammaOpt| < 1` restriction is imposed.
- GammaOpt is written as magnitude and phase in degrees, regardless of the option line's RI setting for S. Nonzero phases use `atan2` in [-180, 180], including the signed-imaginary-zero distinction at the negative real axis. Every complex zero has angle +0. A nonzero imaginary component whose `atan2` result becomes zero, or a nonzero radian angle that becomes zero in degrees, is conservatively rejected for exceeding this double-precision conversion's range. This intermediate-radian guard can reject a case whose exact degree angle would be representable, such as GammaOpt = (1e308, 1e-16). Exact real-axis values, including signed imaginary zero, remain valid. GammaOpt remains referenced to option-line R; per-port `[Reference]` has no effect on either GammaOpt or Rn.

For nonempty noise, `[Number of Noise Frequencies]` follows the S frequency count, before `[Reference]` and `[Network Data]`. After all S data, `[Noise Data]` introduces one five-column line per sample: frequency Hz, Fmin dB, GammaOpt magnitude, GammaOpt degrees, and Rn ohms. `[End]` follows the last noise line.

Frequency, Fmin and Rn use `max_digits10` just like the S scalars. Converting Cartesian GammaOpt to polar form necessarily introduces rounding; bitwise complex round trips and original input angle/winding or decimal spelling are not promised. At the subnormal boundary, errors may be comparable to the smallest representable magnitude. Even when a nonzero angle is representable, angular rounding near an axis can lose or greatly perturb a much smaller Cartesian component on readback. A small error relative to the overall magnitude does not bound the relative error of each component; near the largest finite magnitude, the absolute component error can also be very large. This is distinct from the detected zero-angle underflow rejected above. These representation limits do not authorize changing the stored raw values or adding a physical model.

## Resources and failure behavior

The fixed output payload budget is **64 MiB (67,108,864 bytes)**, including headers, S and noise data, spaces and end marker. All input validation, including noise polar representability, completes first. Serialization uses bounded geometric staging starting at 1 KiB, with no second full-payload copy or separately allocated noise-conversion array; old plus new payload allocations during growth total at most 96 MiB, excluding caller-owned input and small stream/reference bookkeeping. Budget arithmetic is bounded before addition; the final byte count is checked against `streamsize` before conversion. Allocation failure or budget failure does not touch the destination.

Input rejection throws `std::invalid_argument`; exceeding the byte budget throws `std::length_error`; allocation exceptions propagate. An initially failed stream (including eofbit), or a null streambuf, throws `std::ios_base::failure` without changing the initial state. After successful staging, the function calls the destination streambuf's `sputn` **once**. Short writes and streambuf exceptions set badbit and throw `std::ios_base::failure` regardless of the caller's exception mask. The underlying streambuf exception is not preserved.

Direct streambuf output intentionally bypasses ostream sentry behavior, so neither tied streams nor unitbuf cause implicit flushes. The writer never flushes, closes, or clears the destination. Caller locale, flags, precision, width, fill, tie and exception mask are unchanged. I/O failure can leave a prefix; successful return means streambuf acceptance, not atomic replacement, successful later flush/close, or durable storage. The caller owns synchronization and final file I/O checks.

## Format authority and verification

The layout is checked against the original [IBIS Touchstone 2.0 specification](https://www.ibis.org/touchstone_ver2.0/touchstone_ver2_0.pdf), printed pages 6–17 (header, reference, full matrix and ordering rules) and 23–25 (noise data, references and termination). Tests include independently written one-port, nonreciprocal two-port, asymmetric three-port and raw-noise golden byte strings, independent five-column noise extraction, phase/zero/subnormal cases, legacy Rn conversion, reader round trips, invalid input, locale/stream-state preservation, partial/throwing writes and real S/noise 64 MiB budget overflow. The installed consumer spans two translation units and exercises canonical and forwarding headers in full and pure SDK installations relocated to paths containing spaces.

This is format-level interoperability work. No live SystemVue import or simulation compatibility is claimed.
