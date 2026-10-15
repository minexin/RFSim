"""Independent physics, strict transport and immutable six-capture replay gate.

Usage: python -I -S test_amplifier_archive_replay.py --native-probe PROBE
       [--oracle INDEPENDENT_ORACLE] [--fault-oracle NATIVE_REPLAY_FAULT_ORACLE]

The archived vendor comparisons intentionally contain 20 failed checks. Passing
this test suite means those measured comparisons were performed and reported,
not that the nominal cubic model agrees with the vendor at compressed drive.
"""

import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--native-probe", type=Path, required=True)
parser.add_argument("--oracle", type=Path)
parser.add_argument("--fault-oracle", type=Path)
ARGS, remaining = parser.parse_known_args()
PROBE = ARGS.native_probe.resolve()
ORACLE = ARGS.oracle.resolve() if ARGS.oracle else None
FAULT_ORACLE = ARGS.fault_oracle.resolve() if ARGS.fault_oracle else None
sys.argv[1:] = remaining
ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

from native_amplifier_replay import NativeAmplifierReplay
from native_replay import NativeReplayError, atomic_write_report

MAGIC = "RFMODEL_AMPLIFIER_REPLAY"
REQUEST_ID = "0123456789abcdef" * 4
MAX_BIN = 715827882
SINGLE_CASES = (
    ("single-compression-plus09", .9, 100, "sample"),
    ("single-compression-plus09-riso50", .9, 50, "sample"),
    ("single-compression-antenna-plus30", 30., 50, "antenna"),
)
# These hashes predate implementation. They identify available Git/LF bytes,
# not absent raw SystemVue files or the old aggregate's Windows/CRLF bytes.
ARCHIVE_SHA256 = {
    "systemvue-2023-single-compression-plus09-capture.json":
        "cd78a57e8da491c5b46fabb73ac08a19ddfd7203bd855f5e4383f7418975912d",
    "systemvue-2023-single-compression-plus09-riso50-capture.json":
        "52ece1e965936ac4208b2b7ed81e5f1e6b1c3ffb0ef557fffc361e41ca82b824",
    "systemvue-2023-single-compression-antenna-plus30-capture.json":
        "1ad9a31e3a403b1aa691a948267d56000c6ebba8e5cd4c60a01d041dcb0a0d65",
    "systemvue-2023-two-tone-captures.json":
        "f481fb2f03d2fb00396b94a6c842076c57de4b0481f200be79f0df391310d7c2",
    "systemvue-2023-single-compression-plus09.json":
        "a1a653c95ad5fa1fb3556e4b58281c8e217bbffcc38199e92af651c5f1e5ab6e",
    "systemvue-2023-single-compression-plus09-riso50.json":
        "4a7bf673bb52ed2a48bc473fa863534e0cb20e07ef75857f1758e792b75aef98",
    "systemvue-2023-single-compression-antenna-plus30.json":
        "42468815427c7d9b564a4c98aa2b755bb695873fd9f8df9c8be57bb336e69a63",
    "systemvue-2023-two-tone.json":
        "58400a5cb717f477917f2c86386b1f44f6f9407eac8ed9ce98b53c14fdded7c8",
}


def load_script(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), SCRIPTS / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


single = load_script("compare-single-compression.py")
two = load_script("compare-two-tone.py")


def archive(name, root=ROOT):
    return json.loads((root / "validation" / name).read_bytes())


def request(operation, payload, *, version="1", request_id=REQUEST_ID):
    return f"{MAGIC} {version} {operation} {request_id}\n{payload}\n".encode("ascii")


def run_probe(data):
    return subprocess.run([str(PROBE)], input=data, capture_output=True, timeout=10, check=False)


def p1db(provider, incident=.01 + .005j, **options):
    return provider.p1db_fundamental(incident, power_gain_db=20., output_p1db_dbm=20., **options)


def spectrum(provider, amplitudes=None, **options):
    if amplitudes is None:
        amplitudes = {10: .003 + .004j, 11: -.002 + .001j}
    return provider.intercept_amplifier(1e8, amplitudes, power_gain_db=20.,
        input_ip2_dbm=20., input_ip3_dbm=10., **options)


def independent_spectrum(amplitudes, gain_db, ip2_dbm, ip3_dbm):
    """Convolve signed Fourier coefficients in normalized voltage units.

    Equal-tone intercept definitions fix the quadratic and cubic strengths.
    This standard-library oracle neither imports the SDK nor invokes a kernel.
    sqrt(W) input waves need 1/sqrt(2) at each signed RF Fourier frequency;
    conversion back is sqrt(2) for RF and unity for DC.
    """
    gain = 10 ** (gain_db / 20)
    ip2, ip3 = (10 ** ((x - 30) / 10) for x in (ip2_dbm, ip3_dbm))
    coefficients = (0., gain, gain / math.sqrt(2 * ip2), -2 * gain / (3 * ip3))
    signed = {}
    for index, wave in amplitudes.items():
        signed[index] = complex(wave) / math.sqrt(2)
        signed[-index] = complex(wave).conjugate() / math.sqrt(2)
    power, result = {0: 1 + 0j}, {}
    for coefficient in coefficients[1:]:
        product = {}
        for i, left in power.items():
            for j, right in signed.items():
                product[i + j] = product.get(i + j, 0j) + left * right
        power = product
        for index, value in power.items():
            if index >= 0:
                result[index] = result.get(index, 0j) + coefficient * value
    return {index: wave * (math.sqrt(2) if index else 1)
            for index, wave in sorted(result.items()) if wave != 0}


class RawProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_id = json.loads(Path(str(PROBE) + ".manifest.json").read_bytes())["build_id"]

    def assert_rejected(self, data, statuses=("invalid_input",), *, echoed_id=REQUEST_ID):
        first, second = run_probe(data), run_probe(data)
        self.assertEqual((first.returncode, first.stdout, first.stderr),
                         (second.returncode, second.stdout, second.stderr))
        words = first.stdout.decode("ascii").split()
        self.assertEqual(words[:5], [MAGIC, "1", "error", echoed_id, self.build_id])
        self.assertEqual(len(words), 6)
        self.assertIn(words[5], statuses)
        self.assertEqual(first.returncode,
            {"invalid_input": 2, "resource_limit": 3, "numeric_range": 4, "internal_error": 5}[words[5]])
        self.assertLess(len(first.stdout), 512)
        self.assertEqual(first.stderr, b"")

    def test_success_deterministic_crlf_and_explicit_drive(self):
        for operation, payload in (("p1db_fundamental", "20 20 .01 .005 0 0"),
                                   ("p1db_fundamental", "20 20 .01 .005 1 .00025"),
                                   ("intercept_spectrum", "1e8 20 20 10 50 2 10 .001 0 11 0 .001")):
            data = request(operation, payload)
            first = run_probe(data)
            self.assertEqual(first.returncode, 0, first)
            self.assertEqual(first.stderr, b"")
            self.assertEqual(first.stdout, run_probe(data).stdout)
            self.assertEqual(first.stdout, run_probe(data.replace(b"\n", b"\r\n")).stdout)
            self.assertEqual(first.stdout.split()[:6],
                [MAGIC.encode(), b"1", operation.encode(), REQUEST_ID.encode(), self.build_id.encode(), b"ok"])

    def test_version_operation_and_identity_are_strict(self):
        self.assert_rejected(request("p1db_fundamental", "20 20 .001 0 0 0", version="2"),
                             echoed_id="0" * 64)
        self.assert_rejected(request("intercepts", "20 50 0"))
        for identity in ("1", "A" * 64, "g" * 64, "0" * 65):
            with self.subTest(identity=identity):
                self.assert_rejected(request("p1db_fundamental", "20 20 .001 0 0 0",
                                             request_id=identity), echoed_id="0" * 64)
        self.assert_rejected(request("p1db_fundamental", "20 20 .001 0 0 0").replace(
            MAGIC.encode(), b"RFMODEL_NATIVE_REPLAY"), echoed_id="0" * 64)

    def test_p1db_arity_mode_and_drive_guard(self):
        for payload in ("20 20 .001 0 0", "20 20 .001 0 0 0 extra", "20 20 .001 0 2 0",
                        "20 20 .001 0 -1 0", "20 20 .001 0 0.0 0", "20 20 .001 0 0 1e-6",
                        "20 20 .001 0 1 -1", "20 20 .001 0 1 0", "20 20 .01 0 1 1e-5",
                        "20 20 1 0 0 0", "20 20 .001 0 1 1"):
            with self.subTest(payload=payload):
                self.assert_rejected(request("p1db_fundamental", payload))

    def test_spectrum_count_shape_duplicates_and_positive_domain(self):
        for payload in (
            "1e8 20 20 10 50 0", "1e8 20 20 10 50 -1", "1e8 20 20 10 50 3",
            "1e8 20 20 10 50 1.0 10 .001 0", "1e8 20 20 10 50 1 10 .001",
            "1e8 20 20 10 50 1 10 .001 0 extra", "1e8 20 20 10 50 1 0 .001 0",
            "1e8 20 20 10 50 1 -1 .001 0", "1e8 20 20 10 50 1 10.0 .001 0",
            "1e8 20 20 10 50 2 10 .001 0 10 .001 0",
            f"1e8 20 20 10 50 1 {MAX_BIN + 1} .001 0",
            "0 20 20 10 50 1 10 .001 0", "-1 20 20 10 50 1 10 .001 0",
            "1e8 20 20 10 0 1 10 .001 0", "1e8 20 20 10 -50 1 10 .001 0",
        ):
            with self.subTest(payload=payload):
                self.assert_rejected(request("intercept_spectrum", payload),
                                     ("invalid_input", "resource_limit"))

    def test_nonfinite_and_malformed_numbers_reject_in_every_real_field(self):
        for bad in ("nan", "NaN", "inf", "-inf", "Infinity", "0x1p0", "1_0", "1,0", "1e+", ".", "true"):
            for operation, tokens, fields in (
                ("p1db_fundamental", "20 20 .001 0 0 0".split(), (0, 1, 2, 3, 5)),
                ("intercept_spectrum", "1e8 20 20 10 50 1 10 .001 0".split(), (0, 1, 2, 3, 4, 7, 8))):
                for field in fields:
                    mutated = list(tokens)
                    mutated[field] = bad
                    with self.subTest(operation=operation, field=field, bad=bad):
                        self.assert_rejected(request(operation, " ".join(mutated)))

    def test_range_errors_underflow_subnormal_and_integer_limits(self):
        for value in ("1e999", "-1e999", "1e-999", "-1e-999", "2.4703282292062327e-324"):
            with self.subTest(value=value):
                self.assert_rejected(request("p1db_fundamental", f"20 20 {value} 0 0 0"),
                                     ("numeric_range",))
        for value in ("0", "-0", "0e-999", "4.9406564584124654e-324"):
            result = run_probe(request("p1db_fundamental", f"0 20 {value} 0 0 0"))
            self.assertEqual(result.returncode, 0, result)
            self.assertEqual(float(result.stdout.split()[6]), float(value))
        for value in ("2147483648", "18446744073709551616", "-2147483649"):
            self.assert_rejected(request("intercept_spectrum", f"1e8 20 20 10 50 1 {value} .001 0"))
        self.assertEqual(run_probe(request("intercept_spectrum",
            f"1 20 20 10 50 1 {MAX_BIN} .001 0")).returncode, 0)

    def test_eof_binary_extra_request_and_bounded_input(self):
        valid = request("p1db_fundamental", "20 20 .001 0 0 0")
        self.assert_rejected(b"", echoed_id="0" * 64)
        self.assert_rejected(valid + valid)
        for suffix in (b"\x00", b"\xff"):
            self.assert_rejected(valid + suffix, echoed_id="0" * 64)
        self.assert_rejected(request("p1db_fundamental", "1" * 129 + " 20 .001 0 0 0"),
                             ("resource_limit",))
        self.assert_rejected(valid + b" " * 65536, ("resource_limit",), echoed_id="0" * 64)

    def test_raw_byte_budget_crlf_and_dos_eof_are_not_translated(self):
        valid = request("p1db_fundamental", "20 20 .001 0 0 0")
        crlf = valid.replace(b"\n", b"\r\n")
        remaining = 65536 - len(crlf)
        exact = crlf + b"\r\n" * (remaining // 2) + b" " * (remaining % 2)
        self.assertEqual(len(exact), 65536)
        success = run_probe(exact)
        self.assertEqual(success.returncode, 0, success)
        self.assertEqual(success.stdout, run_probe(valid).stdout)
        self.assertNotIn(b"\r", success.stdout, "wire responses must use binary LF output")
        oversized = exact + b" "
        self.assertEqual(len(oversized), 65537)
        self.assert_rejected(oversized, ("resource_limit",), echoed_id="0" * 64)
        hidden_suffix = valid + b"\x1a trailing garbage"
        self.assert_rejected(hidden_suffix)
        # Model CRT text-mode preprocessing independently on every host. These
        # transformed requests succeed, proving both raw-byte negative controls
        # would detect CRLF shrinking or DOS EOF truncation on Windows as well.
        shrunk = oversized.replace(b"\r\n", b"\n")
        self.assertLess(len(shrunk), 65536)
        self.assertEqual(run_probe(shrunk).returncode, 0)
        self.assertEqual(run_probe(hidden_suffix.split(b"\x1a", 1)[0]).returncode, 0)


class IndependentPhysicsTests(unittest.TestCase):
    def setUp(self):
        self.native = NativeAmplifierReplay(PROBE)

    def assert_wave(self, actual, expected, tolerance=3e-13):
        self.assertLessEqual(abs(actual - expected), tolerance * max(abs(expected), 1e-300),
                             (actual, expected))

    def test_p1db_dbm_watts_and_actual_compressed_output_convention(self):
        for gain_db, op1db in ((20., 20.), (30., 60.), (-3., -10.)):
            input_power = 10 ** ((op1db - gain_db + 1 - 30) / 10)
            wave = complex(0, -math.sqrt(input_power))
            output = self.native.p1db_fundamental(wave, power_gain_db=gain_db, output_p1db_dbm=op1db)
            self.assert_wave(output, wave * 10 ** ((gain_db - 1) / 20))
            self.assertAlmostEqual(abs(output) ** 2 / (10 ** ((op1db - 30) / 10)), 1., places=13)
            zero = self.native.p1db_fundamental(0j, power_gain_db=gain_db, output_p1db_dbm=op1db)
            self.assertEqual(zero, 0j)

    def test_complex_phase_and_own_vs_explicit_twice_power(self):
        for wave in (.003 + .004j, -.02 + .001j, .001j):
            power = abs(wave) ** 2
            pin1 = 10 ** (-2.9)
            expected = wave * 10 * (1 - (1 - 10 ** (-.05)) * power / pin1)
            actual = p1db(self.native, wave)
            self.assert_wave(actual, expected)
            self.assert_wave(actual, p1db(self.native, wave, total_incident_power_w=power))
            total = p1db(self.native, wave, total_incident_power_w=2 * power)
            self.assert_wave(total, wave * 10 * (1 - (1 - 10 ** (-.05)) * 2 * power / pin1))
            self.assertLess(abs(total), abs(actual))

    def test_intercept_power_laws_harmonic_factors_and_signed_waves(self):
        power, gain, ip2, ip3 = 1e-6, 10., .1, .01
        wave = math.sqrt(power)
        result = spectrum(self.native, {10: wave, 11: wave})
        self.assert_wave(result[21], gain * power / math.sqrt(ip2))
        self.assert_wave(result[1], result[21])
        self.assert_wave(result[20], result[21] / 2)
        self.assert_wave(result[9], -gain * power ** 1.5 / ip3)
        self.assert_wave(result[30], result[9] / 3)
        self.assert_wave(result[10], gain * wave * (1 - 3 * power / ip3))
        self.assert_wave(result[0], math.sqrt(2) * gain * power / math.sqrt(ip2))
        self.assertAlmostEqual(abs(result[21]) ** 2 / (100 * power ** 2 / ip2), 1., places=13)
        self.assertAlmostEqual(abs(result[9]) ** 2 / (100 * power ** 3 / ip3 ** 2), 1., places=13)

    def test_full_independent_fourier_convolution_phase_unequal_and_collisions(self):
        for waves in ({10: .003 + .004j}, {10: .003 + .004j, 11: -.002 + .001j},
                      {1: .004 - .002j, 2: -.001 + .003j}, {10: 0j}, {10: 0j, 11: .001j}):
            expected = independent_spectrum(waves, 20., 20., 10.)
            for reference in (1., 50., 75.):
                actual = spectrum(self.native, waves, reference_ohms=reference)
                self.assertEqual(set(actual), set(expected))
                self.assertEqual(list(actual), sorted(actual))
                for index, value in expected.items():
                    with self.subTest(waves=waves, reference=reference, bin=index):
                        self.assert_wave(actual[index], value, tolerance=2e-12)

    @unittest.skipIf(ORACLE is None, "standalone long-double oracle not supplied")
    def test_separately_compiled_independent_oracle(self):
        process = subprocess.run([str(ORACLE)], capture_output=True, timeout=15, check=False)
        self.assertEqual(process.returncode, 0, process.stderr)
        lines = process.stdout.decode("ascii").splitlines()
        header = lines.pop(0).split()
        self.assertEqual(header[:2], ["RFMODEL_AMPLIFIER_ORACLE", "1"])
        self.assertEqual(len(lines), int(header[2]))
        self.assertGreaterEqual(len(lines), 8)
        operations, names = set(), set()
        for line in lines:
            words = line.split()
            name, operation, count = words[:3]
            count = int(count)
            payload, expected_count = words[3:3 + count], int(words[3 + count])
            expected = words[4 + count:]
            self.assertEqual(len(expected), expected_count)
            self.assertNotIn(name, names)
            names.add(name)
            operations.add(operation)
            process = run_probe(request(operation, " ".join(payload)))
            self.assertEqual(process.returncode, 0, (name, process))
            actual = process.stdout.decode("ascii").split()[6:]
            self.assertEqual(len(actual), len(expected), name)
            if operation == "p1db_fundamental":
                self.assertEqual(len(expected), 2)
                self.assert_wave(complex(*map(float, actual)), complex(*map(float, expected)),
                                 tolerance=2e-12)
            else:
                self.assertEqual(int(actual[0]), int(expected[0]))
                self.assertEqual(len(expected), 1 + 3 * int(expected[0]))
                for at in range(1, len(expected), 3):
                    self.assertEqual(int(actual[at]), int(expected[at]))
                    self.assert_wave(complex(*map(float, actual[at + 1:at + 3])),
                                     complex(*map(float, expected[at + 1:at + 3])), tolerance=2e-12)
        self.assertEqual(operations, {"p1db_fundamental", "intercept_spectrum"})


class ProviderFailureTests(unittest.TestCase):
    def setUp(self):
        self.native = NativeAmplifierReplay(PROBE)

    def corrupt(self, change, *, operation=spectrum, returncode=None, stderr=None):
        self.native = NativeAmplifierReplay(PROBE)
        original = self.native._run
        def changed(data):
            code, output, errors = original(data)
            self.assertEqual(code, 0)
            return (code if returncode is None else returncode, change(output),
                    errors if stderr is None else stderr)
        with mock.patch.object(self.native, "_run", side_effect=changed):
            with self.assertRaises(NativeReplayError):
                operation(self.native)
        self.assertEqual(self.native.exchanges, [], "malformed executions cannot become evidence")

    @staticmethod
    def replace(position, value):
        def changed(data):
            words = data.split()
            words[position] = value
            return b" ".join(words)
        return changed

    def test_response_header_arity_ascii_and_finite_fields(self):
        for operation in (p1db, spectrum):
            for field, value in ((0, b"RFMODEL_NATIVE_REPLAY"), (1, b"2"), (2, b"wrong"),
                                 (3, b"f" * 64), (4, b"0" * 64), (5, b"invalid_input")):
                with self.subTest(operation=operation.__name__, field=field):
                    self.corrupt(self.replace(field, value), operation=operation)
            for change in (lambda b: b + b" extra", lambda b: b.rsplit(None, 1)[0],
                           lambda b: b"", lambda b: b"\xff", lambda b: b + b"\x00",
                           lambda b: b" " * (16 * 1024 * 1024 + 1)):
                self.corrupt(change, operation=operation)
        for value in (b"nan", b"inf", b"1e999", b"1e-999", b"0x1p0", b"1" * 129):
            self.corrupt(self.replace(6, value), operation=p1db)
            self.corrupt(self.replace(8, value))
        self.corrupt(self.replace(6, b"1e308"), operation=p1db)
        self.corrupt(self.replace(8, b"1e308"))
        self.corrupt(lambda b: b, returncode=1)
        self.corrupt(lambda b: b, stderr=b"unexpected diagnostic\n")

    def test_response_count_order_duplicate_range_and_dc(self):
        for field, value in ((6, b"-1"), (6, b"17"), (6, b"1.0"),
                             (7, b"-1"), (7, b"2147483648"), (7, b"0.0"),
                             (10, b"0"), (10, b"2"), (9, b".001")):
            with self.subTest(field=field, value=value):
                self.corrupt(self.replace(field, value))
        def reverse(data):
            words = data.split()
            rows = [words[at:at + 3] for at in range(7, len(words), 3)]
            return b" ".join(words[:7] + [word for row in reversed(rows) for word in row])
        self.corrupt(reverse)

    def test_invalid_python_inputs_cannot_launch(self):
        calls = (
            lambda: p1db(self.native, True),
            lambda: p1db(self.native, complex(math.nan, 0)),
            lambda: p1db(self.native, total_incident_power_w=True),
            lambda: p1db(self.native, total_incident_power_w=-1),
            lambda: spectrum(self.native, {}),
            lambda: spectrum(self.native, {10: .001, 11: .001, 12: .001}),
            lambda: spectrum(self.native, {True: .001}),
            lambda: spectrum(self.native, {0: .001}),
            lambda: spectrum(self.native, {MAX_BIN + 1: .001}),
            lambda: spectrum(self.native, {10.5: .001}),
            lambda: spectrum(self.native, {10: True}),
            lambda: spectrum(self.native, {10: complex(0, math.inf)}),
            lambda: spectrum(self.native, reference_ohms=0),
        )
        with mock.patch.object(self.native, "_run", side_effect=AssertionError("invalid input launched")):
            for index, call in enumerate(calls):
                with self.subTest(index=index), self.assertRaises((TypeError, ValueError)):
                    call()
        self.assertEqual(self.native.exchanges, [])

    @unittest.skipIf(FAULT_ORACLE is None, "bounded subprocess fault executable not supplied")
    def test_actual_process_timeout_stdout_stderr_and_nonzero(self):
        for mode, expected in (("timeout", "timeout"), ("stdout_limit", "resource_limit"),
                               ("stderr_limit", "resource_limit"), ("nonzero", "execution_failure")):
            with self.subTest(mode=mode), mock.patch.dict(os.environ,
                    {"RFMODEL_REFERENCE_ORACLE_FAULT": mode}):
                provider = NativeAmplifierReplay(FAULT_ORACLE,
                    manifest_path=Path(str(PROBE) + ".manifest.json"),
                    timeout=.05 if mode == "timeout" else 10.)
                with self.assertRaises(NativeReplayError) as caught:
                    p1db(provider)
                self.assertEqual(caught.exception.status, expected)
                self.assertEqual(provider.exchanges, [])

    def test_spawn_failure_and_timeout_validation(self):
        with mock.patch("native_replay.subprocess.Popen", side_effect=OSError("controlled spawn fault")):
            with self.assertRaises(NativeReplayError):
                p1db(self.native)
        self.assertEqual(self.native.exchanges, [])
        for value in (0, -1, 61, math.nan, math.inf, True):
            with self.subTest(timeout=value), self.assertRaises((ValueError, TypeError)):
                NativeAmplifierReplay(PROBE, timeout=value)

    def test_error_status_code_and_operation_must_agree(self):
        for status, code in (("invalid_input", 2), ("resource_limit", 3),
                             ("numeric_range", 4), ("internal_error", 5)):
            def error(request_bytes, status=status, code=code):
                identity = request_bytes.split()[3].decode("ascii")
                build = self.native.execution_identity["build_id"]
                return code, f"{MAGIC} 1 error {identity} {build} {status}\n".encode(), b""
            with self.subTest(status=status), mock.patch.object(self.native, "_run", side_effect=error):
                with self.assertRaises(NativeReplayError) as caught:
                    p1db(self.native)
                self.assertEqual(caught.exception.status, status)
            self.assertEqual(self.native.exchanges, [])
        for bad_code, bad_op, extra, stderr in ((0, "error", "", b""),
                (2, "p1db_fundamental", "", b""), (2, "error", " extra", b""),
                (2, "error", "", b"unexpected diagnostic")):
            def malformed(data):
                identity = data.split()[3].decode()
                build = self.native.execution_identity["build_id"]
                return bad_code, (f"{MAGIC} 1 {bad_op} {identity} {build} invalid_input{extra}\n").encode(), stderr
            with self.subTest(code=bad_code, op=bad_op, extra=extra, stderr=stderr), mock.patch.object(
                    self.native, "_run", side_effect=malformed):
                with self.assertRaises(NativeReplayError):
                    p1db(self.native)
            self.assertEqual(self.native.exchanges, [])

    def test_sidecar_change_or_executable_change_cannot_be_used(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / PROBE.name
            shutil.copy2(PROBE, probe)
            sidecar = Path(str(probe) + ".manifest.json")
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"), sidecar)
            provider = NativeAmplifierReplay(probe)
            sidecar.write_bytes(sidecar.read_bytes() + b" ")
            with self.assertRaises(NativeReplayError):
                p1db(provider)
            self.assertEqual(provider.exchanges, [])
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"), sidecar)
            provider = NativeAmplifierReplay(probe)
            with probe.open("ab") as output:
                output.write(b"changed")
            with self.assertRaises(NativeReplayError):
                p1db(provider)
            self.assertEqual(provider.exchanges, [])

    def test_request_response_hashes_and_mode_mapping(self):
        p1db(self.native)
        p1db(self.native, total_incident_power_w=.00025)
        spectrum(self.native)
        for exchange in self.native.exchanges:
            data = exchange["canonical_request"].encode("ascii")
            words = data.decode("ascii").split()
            operation, request_id = words[2:4]
            payload = " ".join(words[4:]) + "\n"
            self.assertEqual(request_id, hashlib.sha256((operation + "\n" + payload).encode()).hexdigest())
            self.assertEqual(exchange["request_sha256"], hashlib.sha256(data).hexdigest())
            actual = run_probe(data)
            self.assertEqual(actual.returncode, 0)
            self.assertEqual(exchange["response_sha256"], hashlib.sha256(actual.stdout).hexdigest())
        own, explicit, polynomial = [e["canonical_request"].split()[4:] for e in self.native.exchanges]
        self.assertEqual(own, ["20.0", "20.0", "0.01", "0.005", "0", "0.0"])
        self.assertEqual(explicit[-2:], ["1", "0.00025"])
        self.assertEqual([float(x) for x in polynomial[:5]], [1e8, 20., 20., 10., 50.])


class ArchiveReplayTests(unittest.TestCase):
    def test_frozen_all_eight_archive_bytes_and_historical_hash_scope(self):
        for name, expected in ARCHIVE_SHA256.items():
            with self.subTest(archive=name):
                self.assertEqual(hashlib.sha256((ROOT / "validation" / name).read_bytes()).hexdigest(), expected)
        captures = (ROOT / "validation/systemvue-2023-two-tone-captures.json").read_bytes()
        report = archive("systemvue-2023-two-tone.json")
        self.assertNotEqual(hashlib.sha256(captures).hexdigest(), report["captures_sha256"])
        self.assertEqual(hashlib.sha256(captures.replace(b"\n", b"\r\n")).hexdigest(),
                         report["captures_sha256"])
        for stem, _, _, _ in SINGLE_CASES:
            capture = archive("systemvue-2023-" + stem + "-capture.json")
            historical = archive("systemvue-2023-" + stem + ".json")
            self.assertEqual(historical["capture_sha256"], capture["raw_capture_sha256"])

    def test_all_six_measured_cases_42_checks_and_20_nominal_failures(self):
        provider = NativeAmplifierReplay(PROBE)
        singles = [single.compare(provider, archive("systemvue-2023-" + stem + "-capture.json"),
                    power, reverse_isolation_db=riso, profile=profile)
                   for stem, power, riso, profile in SINGLE_CASES]
        doubles = [two.compare(provider, capture, capture["source_power_dbm_per_tone"])
                   for capture in archive("systemvue-2023-two-tone-captures.json")]
        checks = [c for r in singles for c in r["checks"]] + [c for r in doubles
                  for c in r["checks"] + r["direct_fundamental_checks"]]
        self.assertEqual(len(checks), 42)
        self.assertEqual(sum(c["passed"] for c in checks), 22)
        self.assertEqual([r["passed"] for r in singles], [True, True, True])
        self.assertEqual([r["passed"] for r in doubles], [True, False, False])
        self.assertEqual(len(provider.exchanges), 9)
        for report in singles + doubles:
            self.assertEqual(report["relative_tolerance"], 1e-7)
        for report in doubles:
            self.assertTrue(all(c["passed"] for c in report["direct_fundamental_checks"]))
            for value in report["limiter_hypotheses"].values():
                self.assertIs(value["affects_compatibility_verdict"], False)

    def test_nominal_single_input_not_measured_dcp_and_two_tone_total_2p(self):
        provider = NativeAmplifierReplay(PROBE)
        capture = archive("systemvue-2023-single-compression-plus09-capture.json")
        single.compare(provider, capture, .9)
        request_values = provider.exchanges[-1]["canonical_request"].split()[4:]
        power = 10 ** ((.9 - 30) / 10)
        self.assertEqual(float(request_values[2]), math.sqrt(power))
        measured = single.inspect_capture(capture, .9)["measurements"]["DCP"][0]
        self.assertNotEqual(float(request_values[2]), math.sqrt(measured))
        for capture in archive("systemvue-2023-two-tone-captures.json"):
            nominal = capture["source_power_dbm_per_tone"]
            two.compare(provider, capture, nominal)
            polynomial, direct = [e["canonical_request"].split()[4:] for e in provider.exchanges[-2:]]
            power = 10 ** ((nominal - 30) / 10)
            self.assertEqual([float(v) for v in polynomial[:6]], [1e8, 20, 20, 10, 50, 2])
            self.assertEqual([int(polynomial[6]), int(polynomial[9])], [10, 11])
            self.assertEqual([float(polynomial[7]), float(polynomial[10])], [math.sqrt(power)] * 2)
            self.assertEqual(int(direct[4]), 1)
            self.assertEqual(float(direct[5]), 2 * power)

    def test_missing_product_or_observation_never_passes_and_bounds_are_exact(self):
        capture = archive("systemvue-2023-two-tone-captures.json")[0]
        provider = NativeAmplifierReplay(PROBE)
        original = provider.intercept_amplifier
        def missing(*args, **kwargs):
            result = original(*args, **kwargs)
            result.pop(9)
            return result
        with mock.patch.object(provider, "intercept_amplifier", side_effect=missing):
            with self.assertRaises((KeyError, ValueError, NativeReplayError)):
                two.compare(provider, capture, -30)
        for mutation in ("missing_identity", "wrong_boundary", "extra_point", "wrong_order"):
            damaged = copy.deepcopy(capture)
            names = two.vector(damaged, "IDName")
            identifiers = two.vector(damaged, "IDNo")
            at = next(i for i, name in enumerate(names) if name.endswith("[2x(Source.Source1)],RFAmp"))
            target_id = identifiers[at]
            point = two.vector(damaged, "ID2").index(target_id)
            if mutation == "missing_identity":
                names[at] = names[at].replace("Source1", "Unknown")
            elif mutation == "wrong_boundary":
                two.vector(damaged, "F2")[point] += .125
            elif mutation == "wrong_order":
                two.vector(damaged, "F2")[point] = 2e9 - .5
            else:
                for name in ("F2", "P2", "ID2"):
                    node = two.node(damaged, two.SPECTRUM + name)
                    node["data"].append(node["data"][point])
                    node["dimensions"] = [len(node["data"])]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                two.compare(provider, damaged, -30)

    def assert_prediction_delta(self, baseline, changed, expected):
        def key(row):
            return row["case_id"], row["category"], row["index"]
        before = {key(row): row for row in baseline["comparison"]["rows"]}
        after = {key(row): row for row in changed["comparison"]["rows"]}
        self.assertEqual(set(before), set(after))
        self.assertEqual({identity for identity in before
                          if before[identity]["current"] != after[identity]["current"]}, expected)
        baseline_numeric = {key(row) for row in baseline["comparison"]["numeric_differences"]}
        changed_numeric = {key(row) for row in changed["comparison"]["numeric_differences"]}
        self.assertTrue(expected <= changed_numeric)
        self.assertEqual(changed_numeric - expected, baseline_numeric - expected)
        self.assertEqual({key(row) for row in changed["comparison"]["status_differences"]}, expected)

    def test_report_records_current_and_historical_judgments_without_forcing_them(self):
        module = load_script("replay_amplifier_archives.py")
        report = module.replay(PROBE)
        summary = report["summary"]
        for key, value in {"capture_count": 6, "check_count": 42, "passed_count": 22,
                           "failed_count": 20, "vendor_passed": False,
                           "historical_passed_count": 22, "historical_failed_count": 20,
                           "historical_vendor_passed": False}.items():
            self.assertEqual(summary[key], value)
        self.assertEqual([len(report["reports"][key]) for key in ("single", "two_tone")], [3, 3])
        self.assertEqual(len(report["comparison"]["rows"]), 42)
        for row in report["comparison"]["rows"]:
            self.assertIsInstance(row["current"]["passed"], bool)
            self.assertIsInstance(row["historical"]["passed"], bool)
        self.assertEqual(report["comparison"]["status_differences"], [])
        # A changed native prediction must change the actual vendor judgment.
        # Perturb only the first result in memory, without changing any archive,
        # historical observation, scorer tolerance, executable or saved report.
        original = NativeAmplifierReplay.p1db_fundamental
        calls = []
        def changed_prediction(provider, *args, **kwargs):
            result = original(provider, *args, **kwargs)
            calls.append(result)
            return .99 * result if len(calls) == 1 else result
        with mock.patch.object(NativeAmplifierReplay, "p1db_fundamental", changed_prediction):
            changed = module.replay(PROBE)
        self.assertEqual(changed["summary"]["failed_count"], 22)
        self.assertEqual(changed["summary"]["historical_failed_count"], 20)
        self.assertEqual(len(changed["comparison"]["status_differences"]), 2)
        expected = {(SINGLE_CASES[0][0], "checks", index) for index in (0, 1)}
        self.assert_prediction_delta(report, changed, expected)
        # Independently exercise the assertion with an existing one-ULP
        # historical prediction difference. A platform's libm rounding must
        # not be mistaken for an additional effect of the deliberate mutation.
        rows = []
        for case_id, category, index in sorted(expected) + [("rounding-only", "checks", 0)]:
            check = {"rfmodel": 1., "systemvue": 1., "signed_relative_error": 0., "passed": True}
            rows.append({"case_id": case_id, "category": category, "index": index,
                         "current": check, "historical": dict(check)})
        rows[-1]["current"]["rfmodel"] = math.nextafter(1., 2.)
        rows[-1]["current"]["signed_relative_error"] = rows[-1]["current"]["rfmodel"] - 1.
        simulated = {"comparison": {"rows": rows, "numeric_differences": [rows[-1]],
                                    "status_differences": []}}
        perturbed = copy.deepcopy(simulated)
        altered = perturbed["comparison"]["rows"][:2]
        for row in altered:
            row["current"].update(rfmodel=.99, signed_relative_error=.99 - 1., passed=False)
        perturbed["comparison"]["numeric_differences"] += altered
        perturbed["comparison"]["status_differences"] = altered
        self.assert_prediction_delta(simulated, perturbed, expected)
        # Conversely, a modified capture is no longer the frozen six-case suite.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "validation").mkdir()
            for name in ARCHIVE_SHA256:
                shutil.copyfile(ROOT / "validation" / name, root / "validation" / name)
            path = root / "validation/systemvue-2023-single-compression-plus09-capture.json"
            capture = json.loads(path.read_bytes())
            for node in capture["nodes"]:
                if node["path"].endswith("/CGAIN"):
                    node["data"][1] *= 1.1
            path.write_text(json.dumps(capture), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Frozen.*archive bytes"):
                module.replay(PROBE, root=root)


class CliAndIsolationTests(unittest.TestCase):
    def run_cli(self, filename, *arguments, env=None, cwd=None):
        command = [sys.executable, "-I", "-S"]
        if filename in ("compare-single-compression.py", "compare-two-tone.py"):
            # Their frozen source predates -I script-directory setup. Preserve
            # those exact bytes and supply only their own import directory.
            driver = ("import pathlib,runpy,sys; "
                      "sys.path.insert(0,str(pathlib.Path(sys.argv[1]).resolve().parent)); "
                      "sys.argv=sys.argv[1:]; runpy.run_path(sys.argv[0],run_name='__main__')")
            command += ["-c", driver]
        command += [str(SCRIPTS / filename), *(str(v) for v in arguments)]
        return subprocess.run(command, capture_output=True, text=True,
                              timeout=90, check=False, env=env, cwd=cwd)

    def test_new_report_cli_is_explicit_immutable_and_byte_identified(self):
        before = {name: (ROOT / "validation" / name).read_bytes() for name in ARCHIVE_SHA256}
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "replay.json"
            missing = self.run_cli("replay_amplifier_archives.py", "--report", output)
            self.assertNotEqual(missing.returncode, 0)
            self.assertFalse(output.exists())
            success = self.run_cli("replay_amplifier_archives.py", "--native-probe", PROBE, "--report", output)
            self.assertEqual(success.returncode, 0, success.stdout + success.stderr)
            report = json.loads(output.read_bytes())
            self.assertFalse(report["summary"]["vendor_passed"])
            encoded = json.dumps(report)
            self.assertIn(hashlib.sha256(PROBE.read_bytes()).hexdigest(), encoded)
            for name, raw in before.items():
                self.assertIn(hashlib.sha256(raw).hexdigest(), encoded, name)
            saved = output.read_bytes()
            repeated = self.run_cli("replay_amplifier_archives.py", "--native-probe", PROBE, "--report", output)
            self.assertNotEqual(repeated.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)
            for protected in (PROBE, Path(str(PROBE) + ".manifest.json"),
                              ROOT / "validation/systemvue-2023-two-tone.json",
                              ROOT / "validation/systemvue-2023-two-tone-captures.json"):
                original = protected.read_bytes()
                rejected = self.run_cli("replay_amplifier_archives.py", "--native-probe", PROBE,
                                        "--report", protected)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertEqual(protected.read_bytes(), original)
        self.assertEqual({name: (ROOT / "validation" / name).read_bytes() for name in before}, before)

    def test_retired_library_default_never_falls_back(self):
        with self.assertRaisesRegex(RuntimeError, "retired"):
            single.Library(PROBE)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "wrong.json"
            for script, source, extra in (
                ("compare-single-compression.py", "systemvue-2023-single-compression-plus09-capture.json",
                 ("--source-power-dbm", ".9")),
                ("compare-two-tone.py", "systemvue-2023-two-tone-captures.json", ())):
                with self.subTest(script=script):
                    process = self.run_cli(script, PROBE, ROOT / "validation" / source, output, *extra)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn("retired", (process.stdout + process.stderr).lower())
                    self.assertFalse(output.exists())

    def test_frozen_original_checker_bytes_are_unchanged(self):
        # These original source files are also frozen by the separate RFAMP
        # pilot. Provider selection belongs in the new replay facade, never an
        # edit to those historical scorers or their retired command-line paths.
        expected = {
            "compare-single-compression.py":
                "48c2eb810803fb5f659cf80ab06c43f9643e201f98e3d75cd1dc8e63f8541050",
            "compare-two-tone.py":
                "bd3a838ee9d4a1b1584da69eadfc1d050dee903b6d3e1be687c6cc367848605e",
        }
        for filename, digest in expected.items():
            with self.subTest(filename=filename):
                self.assertEqual(hashlib.sha256((SCRIPTS / filename).read_bytes()).hexdigest(), digest)

    def test_old_checker_clis_do_not_silently_accept_native_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            for script, source, extra in (
                ("compare-single-compression.py", "systemvue-2023-single-compression-plus09-capture.json",
                 ("--source-power-dbm", ".9")),
                ("compare-two-tone.py", "systemvue-2023-two-tone-captures.json", ())):
                with self.subTest(script=script):
                    output = Path(tmp) / (script + ".json")
                    capture = ROOT / "validation" / source
                    result = self.run_cli(script, PROBE, capture, output, *extra, "--native-probe", PROBE)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("unrecognized arguments: --native-probe", result.stderr)
                    self.assertNotIn("ModuleNotFoundError", result.stderr)
                    self.assertFalse(output.exists())

    def test_empty_duplicate_or_missing_two_tone_inputs_cannot_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture, output = Path(tmp) / "captures.json", Path(tmp) / "result.json"
            first = archive("systemvue-2023-two-tone-captures.json")[0]
            for data, expected in (([], "Nonempty controlled sweep required"),
                                   ([first, first], "Duplicate per-tone powers"),
                                   ([{}], "source_power_dbm_per_tone")):
                capture.write_text(json.dumps(data), encoding="utf-8")
                result = self.run_cli("compare-two-tone.py", PROBE, capture, output)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertNotIn("ModuleNotFoundError", result.stderr)
                self.assertFalse(output.exists())

    def test_aggregate_replay_manifest_snapshot_mismatch_cannot_publish(self):
        original = NativeAmplifierReplay.manifest
        def changed_digest(provider, *args, **kwargs):
            result = original(provider, *args, **kwargs)
            hashes = result["available_archive_bytes_sha256"]
            hashes[next(iter(hashes))] = "f" * 64
            return result
        with tempfile.TemporaryDirectory() as tmp:
            module = load_script("replay_amplifier_archives.py")
            output = Path(tmp) / "changed-snapshot.json"
            argv = [str(SCRIPTS / "replay_amplifier_archives.py"), "--native-probe", str(PROBE),
                    "--report", str(output)]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    NativeAmplifierReplay, "manifest", changed_digest):
                with self.assertRaises(NativeReplayError):
                    module.main()
            self.assertFalse(output.exists())

    @unittest.skipIf(FAULT_ORACLE is None, "bounded subprocess fault executable not supplied")
    def test_failed_replay_cannot_create_or_replace_any_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fault = root / FAULT_ORACLE.name
            shutil.copy2(FAULT_ORACLE, fault)
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"), Path(str(fault) + ".manifest.json"))
            output = root / "report.json"
            env = dict(os.environ, RFMODEL_REFERENCE_ORACLE_FAULT="nonzero")
            result = self.run_cli("replay_amplifier_archives.py", "--native-probe", fault,
                                  "--report", output, env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())
            output.write_bytes(b"existing evidence must remain unchanged\n")
            result = self.run_cli("replay_amplifier_archives.py", "--native-probe", fault,
                                  "--report", output, env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"existing evidence must remain unchanged\n")
            self.assertFalse(list(root.glob(".native-replay-*")))

    def test_atomic_report_rejects_alias_symlink_existing_and_nonfinite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            protected = root / "capture.json"
            protected.write_bytes(b"original archived bytes\n")
            for path in (protected, root / ".." / root.name / protected.name):
                with self.assertRaises((ValueError, FileExistsError)):
                    atomic_write_report(path, {"changed": True}, protected_paths=[protected])
            output = root / "new.json"
            atomic_write_report(output, {"passed": False}, protected_paths=[protected, PROBE])
            with self.assertRaises(FileExistsError):
                atomic_write_report(output, {})
            with self.assertRaises(ValueError):
                atomic_write_report(root / "nonfinite.json", {"bad": math.nan})
            self.assertFalse((root / "nonfinite.json").exists())
            self.assertEqual(protected.read_bytes(), b"original archived bytes\n")
            if os.name != "nt":
                alias = root / "alias.json"
                alias.symlink_to(protected)
                with self.assertRaises((ValueError, FileExistsError)):
                    atomic_write_report(alias, {})
            self.assertFalse(list(root.glob(".native-replay-*")))

    def test_fresh_isolated_old_fault_suites_and_replay_without_product_tree(self):
        driver = r'''
import importlib.abc
import pathlib
import runpy
import sys
class RejectProduct(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "rfmodel" or fullname.startswith("rfmodel."):
            raise AssertionError("forbidden product import: " + fullname)
        return None
root, filename, probe = map(pathlib.Path, sys.argv[1:4])
assert not (root / "python").exists()
sys.path.insert(0, str(root / "scripts/reference"))
sys.meta_path.insert(0, RejectProduct())
sys.argv = [str(filename), "--native-probe", str(probe)]
if filename.name == "replay_amplifier_archives.py":
    sys.argv += ["--report", str(root / "fresh-report.json")]
try:
    runpy.run_path(str(filename), run_name="__main__")
except SystemExit as exc:
    if exc.code not in (0, None):
        raise
assert not any(n == "rfmodel" or n.startswith("rfmodel.") for n in sys.modules)
print("ACTIVE_PRODUCT_IMPORT_BAN_PASSED")
'''
        with tempfile.TemporaryDirectory(prefix="rfmodel-amplifier-no-product-") as tmp:
            root = Path(tmp)
            scripts = root / "scripts/reference"
            scripts.mkdir(parents=True)
            (root / "validation").mkdir()
            for source in SCRIPTS.glob("*.py"):
                shutil.copyfile(source, scripts / source.name)
            for name in ARCHIVE_SHA256:
                shutil.copyfile(ROOT / "validation" / name, root / "validation" / name)
            probe = root / PROBE.name
            shutil.copy2(PROBE, probe)
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"), Path(str(probe) + ".manifest.json"))
            for filename in ("test_compare_single_compression.py", "test_compare_two_tone.py",
                             "replay_amplifier_archives.py"):
                with self.subTest(filename=filename):
                    result = subprocess.run([sys.executable, "-I", "-S", "-c", driver,
                        str(root), str(scripts / filename), str(probe)], cwd=root,
                        capture_output=True, text=True, timeout=90, check=False)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("ACTIVE_PRODUCT_IMPORT_BAN_PASSED", result.stdout)


if __name__ == "__main__":
    unittest.main()
