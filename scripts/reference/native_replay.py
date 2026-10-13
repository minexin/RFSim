"""Developer-only transport for four archived high-order reference operations.

No product package, ctypes, RF equations, JSON model dispatch, fallback, or native
library discovery is provided here. Records are owned by the independent scorer.
One bounded, versioned ASCII request is sent to a disposable C++ process.
"""
import argparse
import hashlib
import importlib.abc
import importlib.util
import json
import math
import operator
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from itertools import islice

if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from reference_records import (
    AmplifierOperatingPoint, CoherentComponent, CoherentHighOrderAmplifierResponse,
    CoherentPolynomialResponse, CoherentPolynomialTerm, SpectrumKind,
)

MAGIC = "RFMODEL_NATIVE_REPLAY"
PROTOCOL = 1
MAX_REQUEST_BYTES = 64 * 1024
MAX_RESPONSE_BYTES = 16 * 1024 * 1024
MAX_TOKEN_BYTES = 128
MAX_TERMS = 4096
UINT64_MAX = (1 << 64) - 1
INT32_MAX = (1 << 31) - 1
_HEX = re.compile(r"[0-9a-f]{64}\Z")
_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")
_NUMBER = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
_STATUS_CODES = {"invalid_input": 2, "resource_limit": 3, "numeric_range": 4, "internal_error": 5}


class NativeReplayError(RuntimeError):
    """Probe execution or protocol failure; never a numerical comparison result."""
    def __init__(self, message, *, status="execution_failure"):
        super().__init__(message)
        self.status = status


def _sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def _integer(value, lower, upper):
    if isinstance(value, bool):
        raise TypeError("Integer fields cannot be bool")
    value = operator.index(value)
    if not lower <= value <= upper:
        raise ValueError("Integer outside the protocol domain")
    return value


def _number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError("Finite real number required")
    try:
        value = float(value)
    except OverflowError as exc:
        raise ValueError("Real number outside finite double range") from exc
    if not math.isfinite(value):
        raise ValueError("Finite real number required")
    return value


def _bounded(values, maximum, minimum=0):
    values = tuple(islice(iter(values), maximum + 1))
    if not minimum <= len(values) <= maximum:
        raise ValueError("Record count outside the stage-3A domain")
    return values


def _component_tokens(value):
    index = _integer(value.bin, 1, INT32_MAX)
    kind = _integer(value.kind, 0, 2)
    if kind != 0:
        raise ValueError("Stage-3A replay accepts only source-kind inputs")
    bandwidth = _number(value.bandwidth_hz)
    group = _integer(value.coherence_group, 1, UINT64_MAX)
    if isinstance(value.amplitude, bool):
        raise TypeError("Complex wave cannot be bool")
    amplitude = complex(value.amplitude)
    return (index, kind, bandwidth, group, _number(amplitude.real), _number(amplitude.imag))


class _Tokens:
    def __init__(self, raw):
        if len(raw) > MAX_RESPONSE_BYTES:
            raise NativeReplayError("Probe response exceeds byte limit", status="resource_limit")
        if any(value < 33 and value not in (9, 10, 11, 12, 13, 32) or value > 126 for value in raw):
            raise NativeReplayError("Probe response must contain ASCII tokens and whitespace")
        self.values = raw.decode("ascii").split()
        if any(len(value) > MAX_TOKEN_BYTES for value in self.values):
            raise NativeReplayError("Probe response token exceeds limit", status="resource_limit")
        self.offset = 0

    def take(self):
        if self.offset == len(self.values):
            raise NativeReplayError("Truncated probe response")
        value = self.values[self.offset]
        self.offset += 1
        return value

    def integer(self, lower, upper):
        token = self.take()
        if not _INTEGER.fullmatch(token):
            raise NativeReplayError("Malformed integer in probe response")
        value = int(token)
        if not lower <= value <= upper:
            raise NativeReplayError("Probe integer outside permitted range")
        return value

    def number(self):
        token = self.take()
        if not _NUMBER.fullmatch(token):
            raise NativeReplayError("Malformed real in probe response")
        value = float(token)
        # Subnormal doubles are accepted. A nonzero literal rounded to zero is not.
        mantissa = re.split("[eE]", token)[0]
        if not math.isfinite(value) or value == 0 and any(c in "123456789" for c in mantissa):
            raise NativeReplayError("Nonfinite or underflowed probe result")
        return value

    def finish(self):
        if self.offset != len(self.values):
            raise NativeReplayError("Extra tokens in probe response")


def _decode_component(reader, spacing):
    index = reader.integer(1, INT32_MAX)
    kind = SpectrumKind(reader.integer(0, 2))
    bandwidth = reader.number()
    group = reader.integer(1, UINT64_MAX)
    amplitude = complex(reader.number(), reader.number())
    frequency = index * spacing
    norm = amplitude.real * amplitude.real + amplitude.imag * amplitude.imag
    if (bandwidth <= 0 or frequency - bandwidth / 2 < 0
            or not math.isfinite(frequency + bandwidth / 2) or not math.isfinite(norm)):
        raise NativeReplayError("Invalid RF support or wave norm in probe response")
    return CoherentComponent(index, kind, bandwidth, group, amplitude)


def _component_key(component):
    return component.bin, component.kind, component.bandwidth_hz, component.coherence_group


def _decode_point(reader):
    point = AmplifierOperatingPoint(*(reader.number() for _ in range(5)))
    if any(value < 0 for value in point[:3]):
        raise NativeReplayError("Negative native operating-point gain, scale or power")
    return point


def _decode_polynomial(reader, spacing, reserved, request_inputs, maximum_order):
    inputs = tuple(_decode_component(reader, spacing) for _ in range(reader.integer(0, len(request_inputs))))
    keys = [_component_key(component) for component in inputs]
    expected_keys = sorted({_component_key(component) for component in request_inputs})
    if keys != expected_keys or any(c.kind != SpectrumKind.SOURCE for c in inputs):
        raise NativeReplayError("Invalid reduced-input identity or ordering")
    highest = max([reserved] + [c.coherence_group for c in inputs])
    terms, identities, generated_groups = [], set(), set()
    for _ in range(reader.integer(0, MAX_TERMS)):
        order = reader.integer(1, maximum_order)
        indices = tuple(reader.integer(-len(inputs), len(inputs)) for _ in range(order))
        if 0 in indices or tuple(sorted(indices)) != indices:
            raise NativeReplayError("Invalid signed one-based term indices")
        identity = order, indices
        if identity in identities:
            raise NativeReplayError("Duplicate predicted term identity")
        identities.add(identity)
        component = _decode_component(reader, spacing)
        expected_bin = sum((1 if i > 0 else -1) * inputs[abs(i) - 1].bin for i in indices)
        if component.bin != expected_bin:
            raise NativeReplayError("Term bin disagrees with its signed input indices")
        expected_bandwidth = sum(inputs[abs(i) - 1].bandwidth_hz for i in indices)
        if not math.isclose(component.bandwidth_hz, expected_bandwidth, rel_tol=8e-15, abs_tol=0):
            raise NativeReplayError("Term bandwidth disagrees with its contributor support")
        if order == 1:
            if indices[0] <= 0 or _component_key(component) != _component_key(inputs[indices[0] - 1]):
                raise NativeReplayError("Direct term identity differs from its input")
        else:
            kind = SpectrumKind.HARMONIC if indices[0] > 0 and indices[0] == indices[-1] else SpectrumKind.INTERMOD
            if (component.kind != kind or component.coherence_group <= highest
                    or component.coherence_group in generated_groups):
                raise NativeReplayError("Invalid generated term kind or coherence group")
            generated_groups.add(component.coherence_group)
        terms.append(CoherentPolynomialTerm(order, indices, component))
    ordering = [(t.order, t.component.bin, t.input_indices) for t in terms]
    if ordering != sorted(ordering):
        raise NativeReplayError("Probe changed native term ordering")
    return CoherentPolynomialResponse(inputs, tuple(terms))


class NativeReplay:
    """Exactly four calculation methods, backed only by the developer C++ probe."""
    def __init__(self, probe, *, timeout=10.0, manifest_path=None):
        self.probe = Path(probe).resolve(strict=True)
        if not self.probe.is_file():
            raise ValueError("Probe must be a file")
        self.manifest_path = Path(manifest_path or str(self.probe) + ".manifest.json").resolve(strict=True)
        self.timeout = _number(timeout)
        if not 0 < self.timeout <= 60:
            raise ValueError("Probe timeout must be in (0, 60] seconds")
        manifest_raw = self.manifest_path.read_bytes()
        if len(manifest_raw) > MAX_RESPONSE_BYTES:
            raise NativeReplayError("Build manifest exceeds byte limit")
        self.build_manifest = json.loads(manifest_raw)
        if (not isinstance(self.build_manifest, dict)
                or type(self.build_manifest.get("protocol")) is not int
                or self.build_manifest["protocol"] != PROTOCOL
                or not isinstance(self.build_manifest.get("build_id"), str)
                or not _HEX.fullmatch(self.build_manifest["build_id"])):
            raise NativeReplayError("Invalid or unsupported native build manifest")
        # Version 1 uses the exact LF byte envelope produced by CMake. The
        # embedded ID commits to every provenance field, not merely its own
        # echoed label. Any metadata edit requires a new embedded probe ID.
        build_id = self.build_manifest["build_id"]
        prefix = b'{\n  "build_id": "' + build_id.encode("ascii") + b'",\n'
        if (not manifest_raw.startswith(prefix) or not manifest_raw.endswith(b"\n}\n")
                or _sha256(manifest_raw[len(prefix):-3]) != build_id):
            raise NativeReplayError("Native build manifest content does not match its build ID")
        self.execution_identity = {
            "protocol": PROTOCOL,
            "build_id": self.build_manifest["build_id"],
            "executable_sha256": _sha256(self.probe.read_bytes()),
            "build_manifest_sha256": _sha256(manifest_raw),
        }
        self.exchanges = []
        # Freeze the developer scripts before any scorer runs. Reporting must not
        # accidentally attribute old calculations to subsequently edited files.
        self._scorer_snapshot = {
            str(path.resolve()): _sha256(path.read_bytes())
            for path in Path(__file__).parent.glob("*.py")
        }

    def _run(self, request):
        """Bound both pipe readers; stdin is a finite file, avoiding writer deadlock."""
        chunks = [bytearray(), bytearray()]
        failures = []
        with tempfile.TemporaryFile() as source:
            source.write(request)
            source.seek(0)
            try:
                process = subprocess.Popen([str(self.probe)], stdin=source, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, shell=False)
            except OSError as exc:
                raise NativeReplayError("Cannot start native probe") from exc
            def read(pipe, output, limit):
                try:
                    while True:
                        # read() can wait to fill its request even when the
                        # first excess byte is already available. read1() makes
                        # at most one raw read, bounded by the remaining budget
                        # plus the single byte needed to detect overflow.
                        block = pipe.read1(min(65536, limit - len(output) + 1))
                        if not block:
                            return
                        if len(output) + len(block) > limit:
                            failures.append(("resource_limit", "Probe output exceeds byte limit"))
                            process.kill()
                            return
                        output.extend(block)
                except OSError:
                    failures.append(("execution_failure", "Cannot read native probe output"))
                finally:
                    pipe.close()
            readers = [threading.Thread(target=read, args=(pipe, output, limit), daemon=True)
                       for pipe, output, limit in ((process.stdout, chunks[0], MAX_RESPONSE_BYTES),
                                                  (process.stderr, chunks[1], MAX_REQUEST_BYTES))]
            for reader in readers:
                reader.start()
            timed_out = None
            try:
                process.wait(timeout=self.timeout)
            except subprocess.TimeoutExpired as exc:
                timed_out = exc
                process.kill()
                process.wait()
            finally:
                for reader in readers:
                    reader.join(timeout=self.timeout)
            # A deadline may race with the reader's observed overflow/kill.
            # Classify real excess bytes after cleanup rather than masking that
            # evidence with TimeoutExpired. An unexceeded budget stays timeout.
            overflow = next((message for status, message in failures if status == "resource_limit"), None)
            if overflow is not None:
                raise NativeReplayError(overflow, status="resource_limit")
            if timed_out is not None:
                raise NativeReplayError("Native probe timed out", status="timeout") from timed_out
            if any(reader.is_alive() for reader in readers):
                raise NativeReplayError("Native probe output did not close", status="timeout")
            if failures:
                status, message = failures[0]
                raise NativeReplayError(message, status=status)
            return process.returncode, bytes(chunks[0]), bytes(chunks[1])

    def _invoke(self, operation, tokens):
        if operation not in ("intercepts", "operating_point", "polynomial", "highorder"):
            raise ValueError("Unknown native replay operation")
        payload = " ".join(str(value) for value in tokens) + "\n"
        request_id = _sha256((operation + "\n" + payload).encode("ascii"))
        request = f"{MAGIC} {PROTOCOL} {operation} {request_id}\n{payload}".encode("ascii")
        if len(request) > MAX_REQUEST_BYTES or any(len(t) > MAX_TOKEN_BYTES for t in request.split()):
            raise ValueError("Native replay request exceeds protocol limits")
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
            if returned_op != "error" or _STATUS_CODES.get(status) != code:
                raise NativeReplayError("Malformed native probe error record")
            raise NativeReplayError("Native probe rejected request: " + status, status=status)
        if code != 0 or returned_op != operation or stderr:
            raise NativeReplayError("Native probe failed or produced unexpected diagnostics")
        exchange = {"operation": operation, "request_id": request_id,
                    "request_sha256": _sha256(request), "response_sha256": _sha256(stdout),
                    "canonical_request": request.decode("ascii")}
        return reader, exchange

    def _complete(self, reader, exchange, result):
        reader.finish()
        self.exchanges.append(exchange)
        return result

    def polynomial_coefficients_from_intercepts(self, power_gain_db, intercepts, *, reference_ohms=50.0):
        rows = _bounded(intercepts, 10)
        tokens = [_number(power_gain_db), _number(reference_ohms), len(rows)]
        maximum_order = 1
        for row in rows:
            first = _integer(row.first_tone_order, -11, 11)
            second = _integer(row.second_tone_order, -11, 11)
            sign = _integer(row.coefficient_sign, -1, 1)
            if sign == 0:
                raise ValueError("Intercept coefficient sign must be -1 or 1")
            tokens.extend((first, second, _number(row.intercept_dbm), sign,
                           _integer(row.reference, 0, 1)))
            maximum_order = max(maximum_order, abs(first) + abs(second))
        reader, exchange = self._invoke("intercepts", tokens)
        count = reader.integer(2, 12)
        if count != maximum_order + 1:
            raise NativeReplayError("Unexpected intercept coefficient count")
        coefficients = tuple(reader.number() for _ in range(count))
        if coefficients[0] != 0:
            raise NativeReplayError("Nonzero RF polynomial constant coefficient")
        return self._complete(reader, exchange, coefficients)

    def amplifier_operating_point(self, total_incident_power_w, *, power_gain_db, output_p1db_dbm,
                                  output_saturation_dbm, input_ip2_dbm, input_ip3_dbm,
                                  reference_ohms=50.0):
        tokens = tuple(_number(value) for value in (total_incident_power_w, power_gain_db,
                       output_p1db_dbm, output_saturation_dbm, input_ip2_dbm, input_ip3_dbm,
                       reference_ohms))
        reader, exchange = self._invoke("operating_point", tokens)
        point = _decode_point(reader)
        return self._complete(reader, exchange, point)

    def coherent_polynomial(self, spacing_hz, components, voltage_coefficients, *,
                            reference_ohms=50.0, reserved_group_max=0):
        spacing = _number(spacing_hz)
        reserved = _integer(reserved_group_max, 0, UINT64_MAX)
        coefficients = tuple(_number(value) for value in _bounded(voltage_coefficients, 12, 1))
        inputs = _bounded(components, 2)
        tokens = [spacing, _number(reference_ohms), reserved, len(coefficients), *coefficients, len(inputs)]
        for value in inputs:
            tokens.extend(_component_tokens(value))
        reader, exchange = self._invoke("polynomial", tokens)
        result = _decode_polynomial(reader, spacing, reserved, inputs, len(coefficients) - 1)
        if any(coefficients[term.order] == 0 for term in result.terms):
            raise NativeReplayError("Term returned for a zero polynomial coefficient")
        # The kernel omits exactly-zero projected waves, including cancellation
        # and double underflow. This checks required direct identities only; it
        # is not a second polynomial predictor or a numeric comparison oracle.
        direct = {term.input_indices[0] for term in result.terms if term.order == 1}
        expected_direct = {
            index for index, component in enumerate(result.inputs, 1)
            if len(coefficients) > 1 and complex(coefficients[1], 0) * component.amplitude != 0
        }
        if direct != expected_direct:
            raise NativeReplayError("Missing or extra polynomial direct input terms")
        return self._complete(reader, exchange, result)

    def highorder_amplifier(self, spacing_hz, components, nonlinear_voltage_coefficients, *,
                            power_gain_db, output_p1db_dbm, output_saturation_dbm,
                            reference_ohms=50.0, reserved_group_max=0, propagate_distortion=False):
        if type(propagate_distortion) is not bool:
            raise TypeError("propagate_distortion must be bool")
        if propagate_distortion:
            raise ValueError("Stage-3A replay does not propagate distortion")
        spacing = _number(spacing_hz)
        reserved = _integer(reserved_group_max, 0, UINT64_MAX)
        coefficients = tuple(_number(value) for value in _bounded(nonlinear_voltage_coefficients, 10))
        inputs = _bounded(components, 2)
        tokens = [spacing, _number(reference_ohms), reserved, _number(power_gain_db),
                  _number(output_p1db_dbm), _number(output_saturation_dbm), len(coefficients),
                  *coefficients, len(inputs)]
        for value in inputs:
            tokens.extend(_component_tokens(value))
        reader, exchange = self._invoke("highorder", tokens)
        total = reader.number()
        if total < 0:
            raise NativeReplayError("Negative native input power")
        point = _decode_point(reader)
        response = _decode_polynomial(reader, spacing, reserved, inputs, len(coefficients) + 1)
        direct = {term.input_indices[0] for term in response.terms if term.order == 1}
        if direct != set(range(1, len(response.inputs) + 1)):
            raise NativeReplayError("Missing high-order amplifier direct input terms")
        if any(coefficients[term.order - 2] == 0 for term in response.terms if term.order > 1):
            raise NativeReplayError("Term returned for a zero high-order coefficient")
        result = CoherentHighOrderAmplifierResponse(response.inputs, response.terms, total, point)
        return self._complete(reader, exchange, result)

    def manifest(self, archive_paths=(), scorer_paths=()):
        for filename, digest in self._scorer_snapshot.items():
            if _sha256(Path(filename).read_bytes()) != digest:
                raise NativeReplayError("Reference source changed during replay")
        if (_sha256(self.probe.read_bytes()) != self.execution_identity["executable_sha256"]
                or _sha256(self.manifest_path.read_bytes()) != self.execution_identity["build_manifest_sha256"]):
            raise NativeReplayError("Probe or build manifest changed during replay")
        return {
            "schema": "rfmodel.native-reference-replay", "schema_version": 1,
            "classification": "diagnostic_only", "eligible_for_compatibility": False,
            "native_replay_execution": dict(self.execution_identity),
            "build": self.build_manifest,
            "available_archive_bytes_sha256": {str(Path(p)): _sha256(Path(p).read_bytes()) for p in archive_paths},
            "scorer_bytes_sha256": {str(Path(p)): _sha256(Path(p).read_bytes()) for p in scorer_paths},
            "adapter_bytes_sha256": {p.name: _sha256(p.read_bytes()) for p in
                                     (Path(__file__), Path(__file__).with_name("reference_records.py"))},
            "units": {"wave": "sqrt(W)", "power": "W", "frequency_and_bandwidth": "Hz",
                      "reference_impedance": "ohm", "gain": "dB", "intercept": "dBm",
                      "polynomial_coefficients": "voltage-domain c0..cn",
                      "highorder_coefficients": "voltage-domain a2..an"},
            "exchanges": list(self.exchanges),
            "verification_scope": (
                "Only listed available bytes were rehashed. Embedded historical raw/workspace "
                "hashes are retained claims, not verification of unavailable originals."
            ),
        }


class _ForbidRFModel(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "rfmodel" or fullname.startswith("rfmodel."):
            raise ImportError("Product rfmodel import forbidden during native reference replay")
        return None


def assert_no_product_imports():
    if any(name == "rfmodel" or name.startswith("rfmodel.") for name in sys.modules):
        raise NativeReplayError("Native replay requires a fresh process without product imports")


def forbid_product_imports():
    assert_no_product_imports()
    if not any(isinstance(finder, _ForbidRFModel) for finder in sys.meta_path):
        sys.meta_path.insert(0, _ForbidRFModel())


def suite_provider(argv):
    """Consume explicit native CLI selection, retaining the positional legacy path."""
    if len(argv) < 2:
        raise ValueError("Pass --native-probe EXECUTABLE or the legacy LIBRARY path")
    if argv[1] == "--native-probe":
        if len(argv) < 3:
            raise ValueError("--native-probe requires an executable path")
        path = Path(argv[2]).resolve()
        del argv[1:3]
        forbid_product_imports()
        def native():
            assert_no_product_imports()
            return NativeReplay(path)
        native.is_native = True
        return native
    path = Path(argv.pop(1)).resolve()
    def legacy():
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
        from rfmodel import Library
        return Library(path)
    legacy.is_native = False
    return legacy


def atomic_write_report(path, report, *, protected_paths=()):
    """Publish a new report atomically; never replace any existing path or input."""
    destination = Path(path).absolute()
    protected = {Path(value).resolve() for value in protected_paths}
    if destination.resolve() in protected:
        raise ValueError("Report must not overwrite an input or executable")
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Native replay reports must use a new output path")
    raw = (json.dumps(report, indent=2, allow_nan=False) + "\n").encode("utf-8")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=".native-replay-", delete=False) as output:
            temporary = Path(output.name)
            output.write(raw)
            output.flush()
            os.fsync(output.fileno())
        # Hard-link publication is atomic and fails if the destination exists,
        # unlike replace()/rename() which may overwrite another concurrent writer.
        os.link(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    forbid_product_imports()
    provider = NativeReplay(args.probe)
    root = Path(__file__).resolve().parents[2]
    names = ("eleventh-order-captures", "highorder-input-references", "highorder-input-diagnostic",
             "eleventh-order-diagnostic", "highorder-captures", "highorder-diagnostic",
             "spectrum-reduction-captures", "spectrum-reduction-diagnostic")
    paths = {name: root / "validation" / ("systemvue-2023-" + name + ".json") for name in names}
    archive_raw = {name: path.read_bytes() for name, path in paths.items()}
    archives = {name: json.loads(raw.decode("utf-8-sig")) for name, raw in archive_raw.items()}
    reports = {}
    for name, filename, inputs in (
        ("measured_input", "diagnose-highorder-input-reference.py",
         ("eleventh-order-captures", "highorder-input-references")),
        ("nominal_source", "diagnose-eleventh-order.py", ("eleventh-order-captures",)),
        ("fourth_fifth_order", "diagnose-highorder-amplifier.py", ("highorder-captures",)),
        ("spectrum_reduction", "diagnose-spectrum-reduction.py", ("highorder-captures", "spectrum-reduction-captures")),
    ):
        spec = importlib.util.spec_from_file_location("replay_" + name, Path(__file__).with_name(filename))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        reports[name] = module.diagnose(provider, *(archives[key] for key in inputs))
    # Reuse the existing unified model test and every one of its independent
    # evidence assertions. Its highorder calls join this provider's audit trail.
    test_path = Path(__file__).with_name("test_highorder_model_reference.py")
    old_argv = sys.argv
    try:
        sys.argv = [str(test_path), "--native-probe", str(provider.probe)]
        spec = importlib.util.spec_from_file_location("replay_unified_model_test", test_path)
        test_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(test_module)
    finally:
        sys.argv = old_argv
    test_module.PROVIDER = lambda: provider
    tests = unittest.defaultTestLoader.loadTestsFromModule(test_module)
    result = unittest.TestResult()
    tests.run(result)
    if not result.wasSuccessful() or result.testsRun != 1:
        raise NativeReplayError("Unified high-order holdout replay failed: " +
                                repr(result.errors + result.failures))
    scorer_paths = sorted(Path(__file__).parent.glob("*.py"))
    manifest = provider.manifest(paths.values(), scorer_paths)
    if manifest["available_archive_bytes_sha256"] != {
        str(paths[name]): _sha256(raw) for name, raw in archive_raw.items()
    }:
        raise NativeReplayError("Archived input bytes changed during replay")
    manifest["unified_model_test"] = {
        "suite": test_path.name,
        "tests_run": result.testsRun,
        "passed": True,
        "asserted_measured_input_holdout_count": 1413,
        "calibration": "Odd calibration observations excluded by the unchanged test",
    }
    manifest["historical_library_identity"] = {
        name: value.get("library_sha256") for name, value in archives.items()
        if isinstance(value, dict) and "library_sha256" in value}
    manifest["reports"] = reports
    manifest["calibration_roles"] = {
        "odd_coefficients": "fitted from designated calibration only; excluded from odd holdout scoring",
        "measured_inputs": "externally measured fixed-reference source waves; diagnostic only",
        "nominal_source": "unchanged independent baseline; failed even-order statuses retained",
    }
    assert_no_product_imports()
    atomic_write_report(args.report, manifest, protected_paths=[provider.probe, provider.manifest_path,
                                                               *paths.values(), *scorer_paths])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
