"""Two bounded developer-only typed operations for historical amplifier scoring.

The established native_replay runner owns process/pipe limits, identity freezing,
manifest verification and atomic report publication. No library discovery,
product Python bindings, C ABI or fallback is supported.
"""
import math
from pathlib import Path
import sys
from itertools import product

from native_replay import (
    NativeReplay, NativeReplayError, _Tokens, _number, _integer, _sha256,
    _STATUS_CODES, MAX_REQUEST_BYTES, MAX_TOKEN_BYTES, forbid_product_imports,
    assert_no_product_imports,
)

MAGIC = "RFMODEL_AMPLIFIER_REPLAY"
PROTOCOL = 1
MAX_BIN = ((1 << 31) - 1) // 3
MAX_OUTPUT_BINS = 16


class NativeAmplifierReplay(NativeReplay):
    """Explicit provider for the two retired checkers, not a product object API."""

    def _invoke(self, operation, tokens):
        if operation not in ("p1db_fundamental", "intercept_spectrum"):
            raise ValueError("Unknown amplifier archive operation")
        payload = " ".join(str(value) for value in tokens) + "\n"
        request_id = _sha256((operation + "\n" + payload).encode("ascii"))
        request = f"{MAGIC} {PROTOCOL} {operation} {request_id}\n{payload}".encode("ascii")
        if len(request) > MAX_REQUEST_BYTES or any(len(t) > MAX_TOKEN_BYTES for t in request.split()):
            raise ValueError("Amplifier request exceeds protocol limits")
        if (_sha256(self.probe.read_bytes()) != self.execution_identity["executable_sha256"]
                or _sha256(self.manifest_path.read_bytes()) != self.execution_identity["build_manifest_sha256"]):
            raise NativeReplayError("Probe or build manifest changed during replay")
        code, stdout, stderr = self._run(request)
        reader = _Tokens(stdout)
        magic, version, returned_op, returned_id, build_id, status = (reader.take() for _ in range(6))
        if (magic != MAGIC or version != str(PROTOCOL) or returned_id != request_id
                or build_id != self.execution_identity["build_id"]):
            raise NativeReplayError("Probe response version, request ID or build ID mismatch")
        if status != "ok":
            reader.finish()
            if returned_op != "error" or _STATUS_CODES.get(status) != code or stderr:
                raise NativeReplayError("Malformed native amplifier error record")
            raise NativeReplayError("Native probe rejected request: " + status, status=status)
        if code != 0 or returned_op != operation or stderr:
            raise NativeReplayError("Native probe failed or produced unexpected diagnostics")
        exchange = {"operation": operation, "request_id": request_id,
                    "request_sha256": _sha256(request), "response_sha256": _sha256(stdout),
                    "canonical_request": request.decode("ascii")}
        return reader, exchange

    @staticmethod
    def _wave(value):
        if isinstance(value, bool):
            raise TypeError("Complex wave cannot be bool")
        value = complex(value)
        real, imag = _number(value.real), _number(value.imag)
        if not math.isfinite(real * real + imag * imag):
            raise ValueError("Wave norm exceeds finite range")
        return real, imag

    def p1db_fundamental(self, incident, *, power_gain_db, output_p1db_dbm,
                         total_incident_power_w=None):
        real, imag = self._wave(incident)
        mode = int(total_incident_power_w is not None)
        drive = 0.0 if mode == 0 else _number(total_incident_power_w)
        if drive < 0:
            raise ValueError("Total RF drive cannot be negative")
        tokens = (_number(power_gain_db), _number(output_p1db_dbm), real, imag, mode, drive)
        reader, exchange = self._invoke("p1db_fundamental", tokens)
        result = complex(reader.number(), reader.number())
        if not math.isfinite(result.real * result.real + result.imag * result.imag):
            raise NativeReplayError("Native output wave norm exceeds finite range")
        return self._complete(reader, exchange, result)

    def intercept_amplifier(self, spacing_hz, amplitudes, *, power_gain_db, input_ip2_dbm,
                            input_ip3_dbm, reference_ohms=50.0):
        if not isinstance(amplitudes, dict) or not 1 <= len(amplitudes) <= 2:
            raise ValueError("Exactly one or two positive-bin input records required")
        spacing, reference = _number(spacing_hz), _number(reference_ohms)
        if spacing <= 0 or reference <= 0:
            raise ValueError("Positive spacing and reference impedance required")
        rows = sorted((_integer(index, 1, MAX_BIN), self._wave(wave))
                      for index, wave in amplitudes.items())
        if not math.isfinite(rows[-1][0] * 3 * spacing):
            raise ValueError("Spectrum support exceeds finite frequency range")
        tokens = [spacing, _number(power_gain_db), _number(input_ip2_dbm),
                  _number(input_ip3_dbm), reference, len(rows)]
        for index, wave in rows:
            tokens.extend((index, *wave))
        reader, exchange = self._invoke("intercept_spectrum", tokens)
        count = reader.integer(0, MAX_OUTPUT_BINS)
        result = {}
        previous = -1
        for _ in range(count):
            index = reader.integer(0, rows[-1][0] * 3)
            if index <= previous:
                raise NativeReplayError("Duplicate or unordered native output bin")
            previous = index
            wave = complex(reader.number(), reader.number())
            if not math.isfinite(wave.real * wave.real + wave.imag * wave.imag):
                raise NativeReplayError("Native output wave norm exceeds finite range")
            if index == 0 and wave.imag != 0:
                raise NativeReplayError("Native DC wave must be real")
            result[index] = wave
        # Zero inputs and exact cancellations are omitted by the SDK. Validate
        # structural support here; independent oracle tests check full nonzero
        # output, and the scorers require every measured product explicitly.
        signed = [sign * index for index, _ in rows for sign in (-1, 1)]
        expected = {sum(terms) for order in (1, 2, 3)
                    for terms in product(signed, repeat=order) if sum(terms) >= 0}
        if not set(result).issubset(expected):
            raise NativeReplayError("Impossible native polynomial support")
        return self._complete(reader, exchange, result)

    def manifest(self, archive_paths=(), scorer_paths=()):
        result = super().manifest(archive_paths, scorer_paths)
        result.update(schema="rfmodel.amplifier-native-reference-replay", schema_version=1)
        result["native_replay_execution"]["magic"] = MAGIC
        result["adapter_bytes_sha256"][Path(__file__).name] = _sha256(Path(__file__).read_bytes())
        result["units"] = {"wave": "sqrt(W)", "power": "W", "frequency": "Hz",
                           "reference_impedance": "ohm", "gain": "dB", "intercept": "dBm"}
        return result


def suite_provider(argv):
    """Opt in explicitly; the historical library argument remains retired."""
    if len(argv) < 3 or argv[1] != "--native-probe":
        raise ValueError("Python product bindings are retired; pass --native-probe EXECUTABLE")
    path = Path(argv[2]).resolve()
    del argv[1:3]
    forbid_product_imports()
    def native():
        assert_no_product_imports()
        return NativeAmplifierReplay(path)
    return native
