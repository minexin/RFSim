"""Developer-only fixed two-stage cubic recipe. No old protocol or graph extension."""
import math
from itertools import combinations_with_replacement
from pathlib import Path
from typing import NamedTuple

from native_replay import (NativeReplay, NativeReplayError, _Tokens, _number, _sha256,
                           _STATUS_CODES, MAX_TOKEN_BYTES, UINT64_MAX)
from native_source_replay import (NativeSourceReplay, CubicSourceResponse, _component,
    _nonnegative, _drive_consistency, _key)
from reference_records import CoherentPolynomialTerm, SpectrumKind

MAGIC = "RFMODEL_CASCADE_ARCHIVE_REPLAY"
PROTOCOL = 1
MAX_BYTES = 65536

class CombinedRecord(NamedTuple):
    component: object
    factors: tuple

class RootRecord(NamedTuple):
    root_id: int
    source_ordinal: int
    group: int
    bin: int
    bandwidth_hz: float
    role: int

class StageStats(NamedTuple):
    input_power_w: float
    limited_input_power_w: float
    physical_input_count: int
    generated_term_count: int
    direct_contribution_count: int
    output_record_count: int

class CascadeResponse(NamedTuple):
    first: CubicSourceResponse
    second: CubicSourceResponse
    combined: tuple
    roots: tuple
    stage_stats: tuple


def _decode_stage(reader, spacing, supplied, maximum_inputs, maximum_terms):
    drive, limited = _nonnegative(reader), _nonnegative(reader)
    _drive_consistency(drive, supplied)
    if limited > drive * (1 + 2e-14):
        raise NativeReplayError("Cascade limited drive exceeds incident drive")
    inputs = tuple(_component(reader, spacing) for _ in range(reader.integer(1, maximum_inputs)))
    if [_key(c) for c in inputs] != sorted({_key(c) for c in supplied}):
        raise NativeReplayError("Incomplete/duplicate cascade input")
    for c in inputs:
        expected = sum((s.amplitude for s in supplied if _key(s) == _key(c)), 0j)
        if abs(c.amplitude - expected) > 2e-14 * max(abs(expected), 1e-300):
            raise NativeReplayError("Cascade input wave changed")
    highest = max(c.coherence_group for c in inputs)
    identities, generated, terms = set(), set(), []
    for _ in range(reader.integer(1, maximum_terms)):
        order = reader.integer(1, 3)
        indices = tuple(reader.integer(-len(inputs), len(inputs)) for _ in range(order))
        if 0 in indices or indices != tuple(sorted(indices)) or (order, indices) in identities:
            raise NativeReplayError("Invalid/duplicate cascade term factors")
        identities.add((order, indices))
        c = _component(reader, spacing)
        if (c.bin != sum((1 if i > 0 else -1) * inputs[abs(i)-1].bin for i in indices)
                or c.bandwidth_hz != sum(inputs[abs(i)-1].bandwidth_hz for i in indices)):
            raise NativeReplayError("Cascade factor support changed")
        if order == 1:
            if indices[0] <= 0 or _key(c) != _key(inputs[indices[0]-1]):
                raise NativeReplayError("Conducted cascade identity changed")
        else:
            kind = SpectrumKind.HARMONIC if indices[0] > 0 and indices[0] == indices[-1] else SpectrumKind.INTERMOD
            if (any(inputs[abs(i)-1].kind != SpectrumKind.SOURCE for i in indices)
                    or c.kind != kind or c.coherence_group <= highest or c.coherence_group in generated):
                raise NativeReplayError("Secondary generation or invalid generated identity")
            generated.add(c.coherence_group)
        terms.append(CoherentPolynomialTerm(order, indices, c))
    ordering = [(t.order, t.component.bin, t.input_indices) for t in terms]
    if ordering != sorted(ordering):
        raise NativeReplayError("Unordered cascade terms")
    expected = {(1, (i+1,)) for i in range(len(inputs))}
    signed = sorted(sign*(i+1) for i,c in enumerate(inputs)
                    if c.kind == SpectrumKind.SOURCE and c.amplitude != 0 for sign in (-1,1))
    for degree in (2,3):
        expected.update((degree, indices) for indices in combinations_with_replacement(signed, degree)
                        if sum((1 if i>0 else -1)*inputs[abs(i)-1].bin for i in indices) > 0)
    if identities != expected:
        raise NativeReplayError("Incomplete cascade term family")
    return CubicSourceResponse(inputs, tuple(terms), drive, limited)


class NativeCascadeReplay(NativeReplay):
    def _invoke(self, operation, tokens):
        if operation != "cubic_two_stage_v1":
            raise ValueError("Only fixed two-stage cascade recipe is supported")
        payload = " ".join(str(v) for v in tokens) + "\n"
        request_id = _sha256((operation + "\n" + payload).encode("ascii"))
        request = f"{MAGIC} {PROTOCOL} {operation} {request_id}\n{payload}".encode("ascii")
        if len(request) > MAX_BYTES or any(len(t)>MAX_TOKEN_BYTES for t in request.split()):
            raise ValueError("Cascade wire limits")
        self._unchanged()
        code, stdout, stderr = self._run(request)
        if len(stdout)>MAX_BYTES:
            raise NativeReplayError("Cascade response byte limit", status="resource_limit")
        reader = _Tokens(stdout)
        magic, version, returned_op, returned_id, build_id, status = (reader.take() for _ in range(6))
        if (magic != MAGIC or version != str(PROTOCOL) or returned_id != request_id
                or build_id != self.execution_identity["build_id"]):
            raise NativeReplayError("Cascade response identity mismatch")
        if status != "ok":
            reader.finish()
            if returned_op != "error" or _STATUS_CODES.get(status) != code or stderr:
                raise NativeReplayError("Malformed cascade error envelope")
            raise NativeReplayError("Cascade request rejected: " + status, status=status)
        if code != 0 or returned_op != operation or stderr:
            raise NativeReplayError("Cascade process failed or emitted diagnostics")
        return reader, dict(operation=operation, request_id=request_id, request_sha256=_sha256(request),
                            response_sha256=_sha256(stdout), canonical_request=request.decode("ascii"))

    def _unchanged(self):
        if (_sha256(self.probe.read_bytes()) != self.execution_identity["executable_sha256"]
                or _sha256(self.manifest_path.read_bytes()) != self.execution_identity["build_manifest_sha256"]):
            raise NativeReplayError("Cascade probe or manifest changed")

    def two_stage(self, spacing_hz, components, *, second_gain_db):
        spacing, inputs, rows = NativeSourceReplay._inputs(spacing_hz, components)
        gain = _number(second_gain_db)
        if (spacing != 1e8 or not 0 <= gain <= 20 or
            any(c.bin not in (10,11) or c.bandwidth_hz != 1 or c.amplitude == 0 for c in inputs) or
            len({c.bin for c in inputs}) != len(inputs) or
            len({c.coherence_group for c in inputs}) != len(inputs)):
            raise ValueError("Only fixed independent-source cascade recipe is supported")
        reader, exchange = self._invoke("cubic_two_stage_v1", [spacing, gain, len(rows),
                                                                *[v for row in rows for v in row]])
        first = _decode_stage(reader, spacing, inputs, 2, 16)
        second = _decode_stage(reader, spacing, [t.component for t in first.terms], 16, 30)
        roots, ordinals = [], set()
        if reader.integer(1,2) != len(inputs):
            raise NativeReplayError("Incomplete physical root map")
        for i in range(len(inputs)):
            root = RootRecord(reader.integer(1,2), reader.integer(1,len(inputs)),
                              reader.integer(1,UINT64_MAX), reader.integer(1,11), reader.number(),
                              reader.integer(0,0))
            source = inputs[root.source_ordinal-1]
            if (root.root_id != i+1 or root.source_ordinal in ordinals or
                (root.group, root.bin, root.bandwidth_hz) !=
                (source.coherence_group, source.bin, source.bandwidth_hz)):
                raise NativeReplayError("Invalid physical root/source mapping")
            ordinals.add(root.source_ordinal)
            roots.append(root)
        source_map = {_key(c): i+1 for i,c in enumerate(inputs)}
        first_factors = {_key(t.component): tuple(sorted((1 if i>0 else -1)*source_map[_key(first.inputs[abs(i)-1])]
                          for i in t.input_indices)) for t in first.terms}
        expected = {}
        for t in second.terms:
            if t.order == 1:
                factors = first_factors[_key(second.inputs[t.input_indices[0]-1])]
            else:
                factors = tuple(sorted((1 if i>0 else -1)*source_map[_key(second.inputs[abs(i)-1])]
                                       for i in t.input_indices))
            identity = (t.component.bin, t.component.kind, t.component.bandwidth_hz, factors)
            expected[identity] = expected.get(identity, 0j) + t.component.amplitude
        combined, seen, groups = [], set(), set()
        for _ in range(reader.integer(1,16)):
            factors = tuple(reader.integer(-len(inputs),len(inputs)) for _ in range(reader.integer(1,3)))
            c = _component(reader, spacing)
            identity = c.bin, c.kind, c.bandwidth_hz, factors
            if (0 in factors or factors != tuple(sorted(factors)) or identity not in expected or identity in seen
                    or c.coherence_group in groups):
                raise NativeReplayError("Invalid/duplicate combined physical identity")
            # Independent typed execution versus raw sum is only an API/transport
            # consistency check, not the archive vendor oracle or mathematical oracle.
            if abs(c.amplitude - expected[identity]) > 3e-13 * max(abs(expected[identity]),1e-300):
                raise NativeReplayError("Typed combined/raw path inconsistency")
            seen.add(identity)
            groups.add(c.coherence_group)
            combined.append(CombinedRecord(c,factors))
        if seen != set(expected):
            raise NativeReplayError("Incomplete combined physical identities")
        stats = []
        for stage in (first,second):
            row = StageStats(_nonnegative(reader),_nonnegative(reader),reader.integer(1,2),
                             reader.integer(1,16),reader.integer(1,16),reader.integer(1,16))
            for x,y in ((row.input_power_w,stage.total_input_power_w),
                        (row.limited_input_power_w,stage.limited_input_power_w)):
                if not math.isclose(x,y,rel_tol=3e-13,abs_tol=0.):
                    raise NativeReplayError("Typed/raw stage drive mismatch")
            if (row.physical_input_count != len(inputs) or
                row.generated_term_count != sum(t.order>1 for t in stage.terms) or
                row.direct_contribution_count != len(stage.inputs) or
                row.output_record_count != len(first.terms)):
                raise NativeReplayError("Typed stage statistics mismatch")
            stats.append(row)
        self._unchanged()
        return self._complete(reader,exchange,CascadeResponse(first,second,tuple(combined),tuple(roots),tuple(stats)))

    def manifest(self, archive_paths=(), scorer_paths=()):
        result = super().manifest(archive_paths,scorer_paths)
        result.update(schema="rfmodel.cascade-native-reference-replay",schema_version=1)
        result["native_replay_execution"]["magic"] = MAGIC
        result["adapter_bytes_sha256"][Path(__file__).name] = _sha256(Path(__file__).read_bytes())
        result["adapter_bytes_sha256"]["native_source_replay.py"] = _sha256(Path(__file__).with_name("native_source_replay.py").read_bytes())
        return result
