"""Bounded source-only archive transport, independent from the retired v1 domain.

Only process/manifest/atomic-publication infrastructure is reused. RF grouping
and predictions are computed by typed C++; there is no product binding fallback.
"""
import math
import re
from itertools import combinations_with_replacement, islice
from pathlib import Path
from typing import NamedTuple

from native_replay import (NativeReplay, NativeReplayError, _Tokens, _number, _integer,
                           _sha256, _STATUS_CODES, MAX_TOKEN_BYTES, UINT64_MAX,
                           forbid_product_imports, assert_no_product_imports)
from reference_records import CoherentComponent, CoherentPolynomialTerm, SpectrumKind

MAGIC = "RFMODEL_SOURCE_ARCHIVE_REPLAY"
PROTOCOL = 1
MAX_BYTES = 65536
MAX_BIN = ((1 << 31) - 1) // 3
_ID = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.:-]{0,63}\Z")


class SourceReduction(NamedTuple):
    components: tuple
    power_by_bin: dict
    total_power_w: float


class SharedResponse(NamedTuple):
    input_power_w: float
    output: SourceReduction


class CubicSourceResponse(NamedTuple):
    inputs: tuple
    terms: tuple
    total_input_power_w: float
    limited_input_power_w: float


def _bounded(values):
    result = tuple(islice(iter(values), 3))
    if not 1 <= len(result) <= 2:
        raise ValueError("Source archive operations require one or two inputs")
    return result


def _key(c):
    return c.bin, c.kind, c.bandwidth_hz, c.coherence_group


def _wave(value):
    if isinstance(value, bool):
        raise TypeError("Wave cannot be bool")
    wave = complex(value)
    real, imag = _number(wave.real), _number(wave.imag)
    if not math.isfinite(real * real + imag * imag) or (wave != 0 and real * real + imag * imag == 0):
        raise ValueError("Wave norm exceeds finite range")
    return real, imag


def _component_tokens(c, spacing):
    index = _integer(c.bin, 1, MAX_BIN)
    if _integer(c.kind, 0, 2) != 0:
        raise ValueError("Only source-kind input is supported")
    width = _number(c.bandwidth_hz)
    group = _integer(c.coherence_group, 1, UINT64_MAX - 16)
    if width <= 0 or index * spacing - width / 2 < 0 or not math.isfinite(3 * (index * spacing + width / 2)):
        raise ValueError("Invalid source frequency support")
    return index, 0, width, group, *_wave(c.amplitude)


def _component(reader, spacing):
    index, kind = reader.integer(1, MAX_BIN * 3), SpectrumKind(reader.integer(0, 2))
    width, group = reader.number(), reader.integer(1, UINT64_MAX)
    wave = complex(reader.number(), reader.number())
    if (width <= 0 or index * spacing - width / 2 < 0
            or not math.isfinite(index * spacing + width / 2)
            or not math.isfinite(wave.real * wave.real + wave.imag * wave.imag)
            or wave != 0 and wave.real * wave.real + wave.imag * wave.imag == 0):
        raise NativeReplayError("Invalid native source component support or norm")
    return CoherentComponent(index, kind, width, group, wave)


def _nonnegative(reader):
    result = reader.number()
    if result < 0:
        raise NativeReplayError("Negative native power")
    return result


def _drive_consistency(drive, inputs):
    # Validate returned accounting against supplied coherent contributors. This
    # is a wire consistency check, not a second RF prediction or gain model.
    waves = {}
    for component in inputs:
        key = _key(component)
        waves[key] = waves.get(key, 0j) + component.amplitude
    expected = sum(w.real*w.real + w.imag*w.imag for w in waves.values())
    if not math.isfinite(expected) or not math.isclose(drive, expected, rel_tol=2e-14, abs_tol=0.):
        raise NativeReplayError("Native drive accounting disagrees with source contributors")


class NativeSourceReplay(NativeReplay):
    def _invoke(self, operation, tokens):
        if operation not in ("source_groups_v1", "shared_fundamentals_v1", "cubic_sources_v1"):
            raise ValueError("Unsupported source archive operation")
        payload = " ".join(str(value) for value in tokens) + "\n"
        request_id = _sha256((operation + "\n" + payload).encode("ascii"))
        request = f"{MAGIC} {PROTOCOL} {operation} {request_id}\n{payload}".encode("ascii")
        if len(request) > MAX_BYTES or any(len(t) > MAX_TOKEN_BYTES for t in request.split()):
            raise ValueError("Source request exceeds wire limits")
        if (_sha256(self.probe.read_bytes()) != self.execution_identity["executable_sha256"]
                or _sha256(self.manifest_path.read_bytes()) != self.execution_identity["build_manifest_sha256"]):
            raise NativeReplayError("Probe or manifest changed during replay")
        code, stdout, stderr = self._run(request)
        if len(stdout) > MAX_BYTES:
            raise NativeReplayError("Source response exceeds byte limit", status="resource_limit")
        reader = _Tokens(stdout)
        magic, version, returned_op, returned_id, build_id, status = (reader.take() for _ in range(6))
        if (magic != MAGIC or version != str(PROTOCOL) or returned_id != request_id
                or build_id != self.execution_identity["build_id"]):
            raise NativeReplayError("Source response identity mismatch")
        if status != "ok":
            reader.finish()
            if returned_op != "error" or _STATUS_CODES.get(status) != code or stderr:
                raise NativeReplayError("Malformed source error envelope")
            raise NativeReplayError("Native source request rejected: " + status, status=status)
        if code != 0 or returned_op != operation or stderr:
            raise NativeReplayError("Native source process failed or emitted diagnostics")
        exchange = dict(operation=operation, request_id=request_id, request_sha256=_sha256(request),
                        response_sha256=_sha256(stdout), canonical_request=request.decode("ascii"))
        return reader, exchange

    def _complete(self, reader, exchange, result):
        if (_sha256(self.probe.read_bytes()) != self.execution_identity["executable_sha256"]
                or _sha256(self.manifest_path.read_bytes()) != self.execution_identity["build_manifest_sha256"]):
            raise NativeReplayError("Probe or manifest changed during source calculation")
        return super()._complete(reader, exchange, result)

    def assign_source_coherence(self, sources):
        sources = _bounded(sources)
        definitions = []
        for source in sources:
            if (not isinstance(source.source_id, str) or not _ID.fullmatch(source.source_id)
                    or not isinstance(source.reference_clock, str)
                    or source.reference_clock and not _ID.fullmatch(source.reference_clock)):
                raise ValueError("Unsupported source/clock identifier")
            definitions.append((source.source_id, source.reference_clock or "-"))
        if len({s[0] for s in definitions}) != len(definitions):
            raise ValueError("Duplicate source identity")
        reader, exchange = self._invoke("source_groups_v1", [len(sources), *[v for row in definitions for v in row]])
        if reader.integer(1, 2) != len(sources):
            raise NativeReplayError("Incomplete source definition response")
        groups = []
        for definition in definitions:
            if (reader.take(), reader.take()) != definition:
                raise NativeReplayError("Native source definition identity/order changed")
            groups.append(reader.integer(1, len(sources)))
        for i, left in enumerate(sources):
            for j, right in enumerate(sources):
                same = i == j or bool(left.reference_clock) and left.reference_clock == right.reference_clock
                if (groups[i] == groups[j]) != same:
                    raise NativeReplayError("Native source equivalence relation changed")
        return self._complete(reader, exchange, tuple(groups))

    @staticmethod
    def _inputs(spacing_hz, components):
        spacing = _number(spacing_hz)
        if spacing <= 0:
            raise ValueError("Positive spacing required")
        inputs = _bounded(components)
        tokens = [_component_tokens(c, spacing) for c in inputs]
        # Equal physical keys are intentional coherent contributors, not duplicate
        # protocol records: every contributor is preserved until typed reduction.
        return spacing, inputs, tokens

    def compress_coherent_fundamentals(self, spacing_hz, components, *, power_gain_db,
                                       output_p1db_dbm, output_saturation_dbm):
        spacing, inputs, rows = self._inputs(spacing_hz, components)
        reader, exchange = self._invoke("shared_fundamentals_v1", [spacing, _number(power_gain_db),
            _number(output_p1db_dbm), _number(output_saturation_dbm), len(rows), *[v for row in rows for v in row]])
        drive = _nonnegative(reader)
        _drive_consistency(drive, inputs)
        output = tuple(_component(reader, spacing) for _ in range(reader.integer(1, 2)))
        if [_key(c) for c in output] != sorted({_key(c) for c in inputs}):
            raise NativeReplayError("Incomplete or duplicate shared output identity")
        powers = {}
        for _ in range(reader.integer(1, 2)):
            index, power = reader.integer(1, MAX_BIN), _nonnegative(reader)
            if powers and index <= next(reversed(powers)):
                raise NativeReplayError("Duplicate or unordered shared output bin")
            powers[index] = power
        total = _nonnegative(reader)
        if set(powers) != {c.bin for c in output}:
            raise NativeReplayError("Incomplete shared power bins")
        for index, value in powers.items():
            if not math.isclose(value, sum(abs(c.amplitude)**2 for c in output if c.bin == index), rel_tol=2e-14, abs_tol=0.):
                raise NativeReplayError("Shared component/bin powers disagree")
        if not math.isclose(total, sum(powers.values()), rel_tol=2e-14, abs_tol=0.):
            raise NativeReplayError("Shared total power disagrees")
        return self._complete(reader, exchange, SharedResponse(drive, SourceReduction(output, powers, total)))

    def coherent_amplifier(self, spacing_hz, components, *, power_gain_db, output_p1db_dbm,
                           output_saturation_dbm, input_ip2_dbm, input_ip3_dbm,
                           reference_ohms=50., reserved_group_max=0, propagate_distortion=False):
        if propagate_distortion is not False:
            raise ValueError("Source archive replay does not support cascade")
        spacing, request_inputs, rows = self._inputs(spacing_hz, components)
        reference = _number(reference_ohms)
        reserved = _integer(reserved_group_max, 0, UINT64_MAX - 16)
        if reference <= 0:
            raise ValueError("Positive reference required")
        tokens = [spacing, _number(power_gain_db), _number(output_p1db_dbm), _number(output_saturation_dbm),
                  _number(input_ip2_dbm), _number(input_ip3_dbm), reference, reserved, len(rows)]
        reader, exchange = self._invoke("cubic_sources_v1", tokens + [v for row in rows for v in row])
        drive, limited = _nonnegative(reader), _nonnegative(reader)
        _drive_consistency(drive, request_inputs)
        if limited > drive * (1 + 2e-14):
            raise NativeReplayError("Limited drive exceeds incident drive")
        inputs = tuple(_component(reader, spacing) for _ in range(reader.integer(1, 2)))
        if [_key(c) for c in inputs] != sorted({_key(c) for c in request_inputs}):
            raise NativeReplayError("Incomplete or duplicate reduced source identity")
        for component in inputs:
            expected_wave = sum((c.amplitude for c in request_inputs if _key(c) == _key(component)), 0j)
            if abs(component.amplitude - expected_wave) > 2e-14 * max(abs(expected_wave), 1e-300):
                raise NativeReplayError("Reduced input wave disagrees with its contributors")
        highest = max([reserved] + [c.coherence_group for c in inputs])
        terms, identities, generated = [], set(), set()
        for _ in range(reader.integer(1, 16)):
            order = reader.integer(1, 3)
            indices = tuple(reader.integer(-len(inputs), len(inputs)) for _ in range(order))
            if 0 in indices or indices != tuple(sorted(indices)) or (order, indices) in identities:
                raise NativeReplayError("Duplicate or invalid source indices")
            identities.add((order, indices))
            c = _component(reader, spacing)
            expected_bin = sum((1 if i > 0 else -1) * inputs[abs(i)-1].bin for i in indices)
            width = sum(inputs[abs(i)-1].bandwidth_hz for i in indices)
            if c.bin != expected_bin or not math.isclose(c.bandwidth_hz, width, rel_tol=8e-15):
                raise NativeReplayError("Source term support disagrees with factors")
            if order == 1:
                if indices[0] <= 0 or _key(c) != _key(inputs[indices[0]-1]):
                    raise NativeReplayError("Direct source identity changed")
            else:
                kind = SpectrumKind.HARMONIC if indices[0] > 0 and indices[0] == indices[-1] else SpectrumKind.INTERMOD
                if c.kind != kind or c.coherence_group <= highest or c.coherence_group in generated:
                    raise NativeReplayError("Generated kind/group identity changed")
                generated.add(c.coherence_group)
            terms.append(CoherentPolynomialTerm(order, indices, c))
        ordering = [(t.order, t.component.bin, t.input_indices) for t in terms]
        if ordering != sorted(ordering):
            raise NativeReplayError("Unordered source terms")
        expected = {(1, (i+1,)) for i in range(len(inputs))}
        signed = sorted(sign*(i+1) for i,c in enumerate(inputs) if c.amplitude != 0 for sign in (-1,1))
        for order in (2,3):
            for indices in combinations_with_replacement(signed, order):
                if sum((1 if i>0 else -1)*inputs[abs(i)-1].bin for i in indices) > 0:
                    expected.add((order,indices))
        # With finite nonzero model coefficients, every positive-frequency signed
        # factor multiset must be present, even at the same bin as another origin.
        if identities != expected:
            raise NativeReplayError("Incomplete source term family")
        return self._complete(reader, exchange, CubicSourceResponse(inputs, tuple(terms), drive, limited))

    def manifest(self, archive_paths=(), scorer_paths=()):
        result = super().manifest(archive_paths, scorer_paths)
        result.update(schema="rfmodel.source-native-reference-replay", schema_version=1)
        result["native_replay_execution"]["magic"] = MAGIC
        result["adapter_bytes_sha256"][Path(__file__).name] = _sha256(Path(__file__).read_bytes())
        result["units"] = dict(wave="sqrt(W)", power="W", frequency="Hz", reference_impedance="ohm",
                               gain="dB", intercept="dBm", cw_direct_width_hz=1)
        return result
