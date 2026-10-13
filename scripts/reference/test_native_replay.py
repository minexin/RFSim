"""Adversarial transport and isolated archived-replay gate; no product imports.

Usage: python test_native_replay.py PROBE DIRECT_KERNEL_ORACLE
Both executables are developer-only targets linked to the header-only SDK.
"""

import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

PROBE = Path(sys.argv.pop(1)).resolve()
ORACLE = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_replay import NativeReplay, NativeReplayError, atomic_write_report
from reference_records import CoherentComponent, InterceptReference, SpectrumKind, TwoToneIntercept

MAGIC = "RFMODEL_NATIVE_REPLAY"
REQUEST_ID = "0123456789abcdef" * 4
MAX_UINT64 = (1 << 64) - 1
SUITES = (
    "test_highorder_model_reference.py",
    "test_highorder_input_reference.py",
    "test_diagnose_eleventh_order.py",
    "test_diagnose_highorder_amplifier.py",
    "test_diagnose_spectrum_reduction.py",
    "test_diagnose_am_pm_reference.py",
    "test_diagnose_am_pm_parameter_study.py",
    "test_systemvue_am_pm_adapter.py",
)
PURE_PYTHON_SUITES = (
    "test_archive_am_pm_datapoints_zero_study.py",
    "test_archive_am_pm_datapoints_zero_acquisition.py",
    "test_diagnose_am_pm_datapoints_zero_study.py",
    "test_prepare_am_pm_datapoints_raw_axis_study.py",
    "test_diagnose_am_pm_datapoints_raw_axis_study.py",
    "test_archive_am_pm_datapoints_raw_axis_acquisition.py",
    "test_report_am_pm_datapoints_raw_axis_study.py",
    "test_prepare_am_pm_datapoints_order_study.py",
    "test_diagnose_am_pm_datapoints_order_study.py",
)
DATAPOINTS_ZERO_FIXTURES = (
    "am-pm-datapoints-zero-plan-20261013.json",
    "am-pm-datapoints-observations-20261012.json",
    "am-pm-existing-reference-evidence-20261012.json",
    "am-pm-datapoints-raw-axis-plan-20261013.json",
    "am-pm-datapoints-order-plan-20261014.json",
)
# Listed artifacts are mandatory: staging must never silently skip a missing
# source file or imply that synthetic tests replay newly acquired observations.
DATAPOINTS_ZERO_RESULT_FIXTURES = (
    "am-pm-datapoints-zero-captures-20261013.json",
    "am-pm-datapoints-zero-observations-20261013.json",
    "am-pm-datapoints-zero-diagnostic-20261013.json",
    "am-pm-datapoints-zero-workflow-repairs-20261013.json",
    "am-pm-datapoints-zero-restoration-20261013.json",
)
DATAPOINTS_RAW_AXIS_RESULT_FIXTURES = (
    "am-pm-datapoints-raw-axis-workflow-20261013.json",
    "am-pm-datapoints-raw-axis-captures-20261013.json",
    "am-pm-datapoints-raw-axis-observations-20261013.json",
    "am-pm-datapoints-raw-axis-diagnostic-20261013.json",
)
# Frozen before stage-3A implementation; these are archival identities, not
# hashes regenerated from the files under test. Keep the historical bytes.
ARCHIVE_SHA256 = {
    "systemvue-2023-eleventh-order-captures.json":
        "553ebdca52dec654a2e01fe77d47104cede890d2f15b69444ea5ee1bfcc4f275",
    "systemvue-2023-highorder-input-references.json":
        "39a5beb695509e21e8b8c6899d5d65a2c25ba1cf01a4b32c52fae1c81cf0d2e0",
    "systemvue-2023-highorder-input-diagnostic.json":
        "d1da5cc4e78a4608a36ed36de3d1e5b037fc12cd07791f78c58e0dde02201149",
    "systemvue-2023-eleventh-order-diagnostic.json":
        "52b54d0f28e8ccb2257497306fa1a7311bf060fbad9efb5358cb582a8a499a36",
    "systemvue-2023-highorder-captures.json":
        "4592a5ff6651836ab17aaf2e39b6809746da647c9856c71c696b392d4d0a5b4e",
    "systemvue-2023-highorder-diagnostic.json":
        "d9b052cfdb2009cc0405ee4ee84660d2bb107fefa4a9d3095d812b52237b6f51",
    "systemvue-2023-spectrum-reduction-captures.json":
        "b6a5b210b97169106706a563aba6c7ce822fee88829899905c86e7ca29fcf2d5",
    "systemvue-2023-spectrum-reduction-diagnostic.json":
        "aec3915d551b56ce3c7cd0d1155a933eee11662e55c69820f8ff514c14383be4",
}
ARCHIVES = tuple(ARCHIVE_SHA256)


def request(operation, payload, *, version="1", request_id=REQUEST_ID):
    return f"{MAGIC} {version} {operation} {request_id}\n{payload}\n".encode("ascii")


def run_probe(data):
    return subprocess.run([str(PROBE)], input=data, capture_output=True, timeout=10, check=False)


def component_record(row):
    return CoherentComponent(row[0], SpectrumKind(row[1]), row[2], row[3], complex(*row[4]))


def component_values(value):
    return [value.bin, int(value.kind), value.bandwidth_hz, value.coherence_group,
            [value.amplitude.real, value.amplitude.imag]]


def response_values(value):
    result = {
        "inputs": [component_values(c) for c in value.inputs],
        "terms": [[t.order, list(t.input_indices), component_values(t.component)]
                  for t in value.terms],
    }
    if hasattr(value, "total_input_power_w"):
        result["total_input_power_w"] = value.total_input_power_w
        result["operating_point"] = list(value.operating_point)
    return result


class RawProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_id = json.loads(Path(str(PROBE) + ".manifest.json").read_text())["build_id"]

    def assert_rejected(self, data, statuses=("invalid_input",), *, echoed_id=REQUEST_ID):
        first = run_probe(data)
        second = run_probe(data)
        self.assertEqual((first.returncode, first.stdout, first.stderr),
                         (second.returncode, second.stdout, second.stderr),
                         "same invalid request must have byte-identical deterministic failure")
        expected_codes = {"invalid_input": 2, "resource_limit": 3,
                          "numeric_range": 4, "internal_error": 5}
        words = first.stdout.decode("ascii").split()
        self.assertEqual(len(words), 6, first)
        self.assertEqual(words[:3], [MAGIC, "1", "error"])
        self.assertEqual(words[3], echoed_id)
        self.assertEqual(words[4], self.build_id)
        self.assertIn(words[5], statuses)
        self.assertEqual(first.returncode, expected_codes[words[5]])
        self.assertLess(len(first.stdout), 512)

    def test_deterministic_success_and_crlf(self):
        data = request("intercepts", "10 50 2 1 1 30 1 0 2 -1 20 -1 0")
        results = [run_probe(data) for _ in range(3)]
        self.assertTrue(all(r.returncode == 0 for r in results), results)
        self.assertTrue(all(r.stdout == results[0].stdout for r in results))
        self.assertEqual(run_probe(data.replace(b"\n", b"\r\n")).stdout, results[0].stdout)
        self.assertEqual(results[0].stdout.split()[:6],
                         [MAGIC.encode(), b"1", b"intercepts", REQUEST_ID.encode(),
                          self.build_id.encode(), b"ok"])

    def test_unknown_version_operation_and_bad_identity(self):
        self.assert_rejected(request("intercepts", "10 50 0", version="2"),
                             echoed_id="0" * 64)
        self.assert_rejected(request("unrecognized", "10 50 0"))
        for identity in ("1", "A" * 64, "g" * 64, "0" * 65):
            with self.subTest(identity=identity):
                self.assert_rejected(request("intercepts", "10 50 0", request_id=identity),
                                     echoed_id="0" * 64)
        self.assert_rejected(b"WRONG 1 intercepts " + REQUEST_ID.encode() + b" 10 50 0\n",
                             echoed_id="0" * 64)

    def test_am_pm_invalid_power_reference_and_payload(self):
        for payload in ("-1 -5 10", "0.01 -5 99999", "0.01 -5 -99999",
                        "0.01 -5", "0.01 -5 10 extra"):
            for operation in ("am_pm", "systemvue2023_am_pm"):
                with self.subTest(payload=payload, operation=operation):
                    self.assert_rejected(request(operation, payload))

    def test_strict_decimal_tokens(self):
        for value in ("nan", "NaN", "inf", "Infinity", "-inf", "0x1p0", "1e", "1e+",
                      "1.2.3", "true", "1_0", "1,0", "++1", ".", "+", "--0"):
            with self.subTest(value=value):
                self.assert_rejected(request("intercepts", f"{value} 50 0"))
        for value in ("1e999", "-1e999", "1e-999", "-1e-999"):
            with self.subTest(value=value):
                self.assert_rejected(request("intercepts", f"{value} 50 0"),
                                     ("numeric_range",))
        for value in ("0", "-0", "+0.0", "0e-999", ".0", "0.", "0E+5"):
            with self.subTest(value=value):
                self.assertEqual(run_probe(request("intercepts", f"{value} 50 0")).returncode, 0)
        # The smallest representable subnormal is distinct from literal underflow.
        for value in ("4.9406564584124654e-324", "-4.9406564584124654e-324"):
            with self.subTest(subnormal=value):
                self.assertEqual(run_probe(request("intercepts", f"{value} 50 0")).returncode, 0)
        self.assert_rejected(request("intercepts", "2.4703282292062327e-324 50 0"),
                             ("numeric_range",))

    def test_signed_subnormal_roundtrip_and_range_boundaries(self):
        # Exercise the parsed value, rather than only accepting a tiny gain whose
        # final exp() rounds to one. These direct waves retain the literal sign.
        for magnitude in ("4.9406564584124654e-324", "9.8813129168249309e-324",
                          "2.225073858507201e-308", "2.2250738585072012e-308",
                          "2.2250738585072014e-308"):
            for sign in ("", "+", "-"):
                value = sign + magnitude
                with self.subTest(value=value):
                    result = run_probe(request(
                        "polynomial", f"1e6 50 0 2 0 {value} 1 10 0 100 1 1 0"))
                    self.assertEqual(result.returncode, 0, result.stdout)
                    words = result.stdout.split()
                    self.assertEqual(words[13:16], [b"1", b"1", b"1"])
                    self.assertEqual(float(words[-2]), float(value))
                    self.assertEqual(math.copysign(1., float(words[-2])),
                                     math.copysign(1., float(value)))
                    self.assertEqual(float(words[-1]), 0.)
        # Empty input isolates parsing from RF amplitude overflow. Finite maximum
        # is accepted; overflow and either sign of rounded-to-zero are rejected.
        for value in ("1.7976931348623157e308", "-1.7976931348623157e308"):
            self.assertEqual(run_probe(request(
                "polynomial", f"1e6 50 0 2 0 {value} 0")).returncode, 0)
        for value in ("1.7976931348623159e308", "-1.7976931348623159e308",
                      "2.4703282292062327e-324", "-2.4703282292062327e-324",
                      "1e-999", "-1e-999"):
            self.assert_rejected(request("polynomial", f"1e6 50 0 2 0 {value} 0"),
                                 ("numeric_range",))

    def test_counts_integer_overflow_and_truncation(self):
        for count in ("-1", "1.0", "true", "18446744073709551616", "9999999999999999999999"):
            with self.subTest(count=count):
                self.assert_rejected(request("intercepts", f"10 50 {count}"))
        self.assert_rejected(request("intercepts", "10 50 11"), ("resource_limit",))
        self.assert_rejected(request("intercepts", "10 50 1 1 1 30 1"))
        self.assert_rejected(request("intercepts", "10 50 0 extra"))
        self.assert_rejected(request("intercepts", "10 50 1 2147483648 1 30 1 0"))
        self.assert_rejected(request("intercepts", "10 50 1 -2147483649 1 30 1 0"))
        self.assert_rejected(request("polynomial", "1e6 50 18446744073709551616 1 0 0"))
        self.assert_rejected(request("polynomial", "1e6 50 -1 1 0 0"))
        self.assert_rejected(request("polynomial", "1e6 50 0 0"))
        self.assert_rejected(request("polynomial", "1e6 50 0 13"), ("resource_limit",))
        self.assert_rejected(request("highorder", "1e6 50 0 10 20 23 11"), ("resource_limit",))
        self.assert_rejected(request("polynomial", "1e6 50 0 1 0 3"), ("resource_limit",))

    def test_request_byte_and_token_bounds(self):
        self.assert_rejected(request("intercepts", "1" * 129 + " 50 0"), ("resource_limit",))
        self.assert_rejected(request("intercepts", "10 50 0") + b" " * 65536,
                             ("resource_limit",), echoed_id="0" * 64)
        self.assert_rejected(request("intercepts", "10 50 0") + b"\x00", echoed_id="0" * 64)
        self.assert_rejected(request("intercepts", "10 50 0") + b"\xff", echoed_id="0" * 64)

    def test_intercepts_reject_invalid_domains(self):
        for row in ("0 1 30 1 0", "11 1 30 1 0", "6 6 30 1 0", "1 1 30 0 0",
                    "1 1 30 2 0", "1 1 30 1 2", "-2147483648 1 30 1 0"):
            with self.subTest(row=row):
                self.assert_rejected(request("intercepts", f"10 50 1 {row}"))
        self.assert_rejected(request("intercepts", "10 50 2 1 1 30 1 0 1 -1 31 1 0"))
        for z in ("0", "-50"):
            self.assert_rejected(request("intercepts", f"10 {z} 0"))
        self.assert_rejected(request("intercepts", "1e308 50 0"), ("numeric_range",))

    def test_component_domains_groups_and_numeric_support(self):
        base = ["10", "0", "1", "7", ".01", ".003"]
        for position, value in ((0, "0"), (0, "-1"), (0, "2147483648"), (1, "1"),
                                (1, "2"), (1, "3"), (2, "0"), (2, "-1"), (2, "3e8"),
                                (3, "0"), (3, "-1"), (3, "18446744073709551616"),
                                (4, "1e308"), (5, "nan")):
            row = list(base)
            row[position] = value
            with self.subTest(position=position, value=value):
                self.assert_rejected(request("polynomial", "1e6 50 0 2 0 1 1 " + " ".join(row)),
                                     ("invalid_input", "numeric_range"))
        for spacing, z in (("0", "50"), ("-1", "50"), ("1e6", "0"), ("1e6", "-50"),
                           ("1e308", "50")):
            self.assert_rejected(request("polynomial", f"{spacing} {z} 0 2 0 1 1 " + " ".join(base)),
                                 ("invalid_input", "numeric_range"))
        self.assert_rejected(request("polynomial", "1e6 50 0 2 1 1 0"))
        self.assert_rejected(request("polynomial", f"1e6 50 {MAX_UINT64} 3 0 0 1 1 " + " ".join(base)),
                             ("numeric_range",))
        self.assert_rejected(request("polynomial", "1 50 0 3 0 0 1 1 2147483647 0 1 7 .01 0"),
                             ("numeric_range",))
        self.assert_rejected(request("polynomial", "1e6 50 0 3 0 0 1e308 1 10 0 1 7 .01 0"),
                             ("numeric_range",))

    def test_operating_point_invalid_domain(self):
        for payload in ("-1 10 20 23 30 20 50", "0 10 20 19 30 20 50",
                        "0 10 20 23 30 20 0"):
            with self.subTest(payload=payload):
                self.assert_rejected(request("operating_point", payload))


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.native = NativeReplay(PROBE)
        self.sources = (CoherentComponent(10, SpectrumKind.SOURCE, 1., 7, .01 + .003j),
                        CoherentComponent(13, SpectrumKind.SOURCE, 2., 9, .02 - .001j))

    def invoke_polynomial(self):
        return self.native.coherent_polynomial(1e6, self.sources, [0., 1., .5], reserved_group_max=100)

    def corrupt_response(self, mutation, method=None, *, returncode=0):
        def fake_run(data):
            actual = run_probe(data)
            self.assertEqual(actual.returncode, 0, actual.stderr)
            return returncode, mutation(actual.stdout), b""
        with mock.patch.object(self.native, "_run", side_effect=fake_run):
            with self.assertRaises(NativeReplayError):
                (method or self.invoke_polynomial)()

    def test_provider_matches_direct_cpp_kernel_fields(self):
        process = subprocess.run([str(ORACLE)], capture_output=True, timeout=30, check=True)
        cases = json.loads(process.stdout)
        self.assertEqual({case["operation"] for case in cases},
                         {"intercepts", "operating_point", "polynomial", "highorder", "am_pm"})
        for case in cases:
            with self.subTest(name=case["name"], drive=case.get("drive")):
                operation = case["operation"]
                if operation == "intercepts":
                    definitions = [TwoToneIntercept(*r[:4], InterceptReference(r[4]))
                                   for r in case["definitions"]]
                    actual = list(self.native.polynomial_coefficients_from_intercepts(
                        case["gain"], definitions, reference_ohms=case["z"]))
                elif operation == "am_pm":
                    actual = list(self.native.am_pm_response(case["power"],
                        peak_degrees_per_db=case["peak"], output_reference_dbm=case["reference"]))
                elif operation == "operating_point":
                    actual = list(self.native.amplifier_operating_point(
                        case["drive"], power_gain_db=10., output_p1db_dbm=20.,
                        output_saturation_dbm=23., input_ip2_dbm=30., input_ip3_dbm=20.,
                        reference_ohms=case["z"]))
                else:
                    inputs = [component_record(c) for c in case["inputs"]]
                    kwargs = dict(reference_ohms=case["z"], reserved_group_max=case["reserved"])
                    if operation == "polynomial":
                        value = self.native.coherent_polynomial(case["spacing"], inputs,
                                                               case["coefficients"], **kwargs)
                    else:
                        value = self.native.highorder_amplifier(
                            case["spacing"], inputs, case["coefficients"], power_gain_db=10.,
                            output_p1db_dbm=20., output_saturation_dbm=23., **kwargs)
                    actual = response_values(value)
                # Both programs use max_digits10 from the same compiler/headers.
                # Exact equality is intentional for this same-build transport oracle,
                # not a cross-platform floating-point requirement.
                self.assertEqual(actual, case["expected"])

    def test_adapter_input_validation(self):
        for spacing in (True, math.nan, math.inf, -math.inf):
            with self.subTest(spacing=spacing), self.assertRaises((ValueError, TypeError)):
                self.native.coherent_polynomial(spacing, self.sources, [0., 1.])
        for reserved in (True, -1, MAX_UINT64 + 1, 1.5):
            with self.subTest(reserved=reserved), self.assertRaises((ValueError, TypeError)):
                self.native.coherent_polynomial(1e6, self.sources, [0., 1.], reserved_group_max=reserved)
        for coefficients in ([], [0.] * 13, [0., math.inf], [0., math.nan], [0., True]):
            with self.subTest(coefficients=coefficients), self.assertRaises((ValueError, TypeError)):
                self.native.coherent_polynomial(1e6, self.sources, coefficients)
        with self.assertRaises((ValueError, TypeError)):
            self.native.coherent_polynomial(1e6, self.sources * 2, [0., 1.])
        with self.assertRaises((ValueError, TypeError)):
            self.native.highorder_amplifier(1e6, self.sources, [], power_gain_db=10.,
                output_p1db_dbm=20., output_saturation_dbm=23., propagate_distortion=True)

    def test_am_pm_response_shape_and_finite_fields(self):
        for method in (self.native.am_pm_response, self.native.systemvue2023_am_pm_response):
            invoke = lambda: method(.01, peak_degrees_per_db=-5, output_reference_dbm=10)
            for mutation in (lambda b: b + b" extra", lambda b: b.rsplit(None, 1)[0],
                             lambda b: b.rsplit(None, 1)[0] + b" nan",
                             lambda b: b.rsplit(None, 1)[0] + b" 1e999"):
                with self.subTest(mutation=mutation, method=method):
                    self.corrupt_response(mutation, method=invoke)

    def test_malformed_response_header_number_and_bounds(self):
        def replace(position, value):
            def change(data):
                words = data.split()
                words[position] = value
                return b" ".join(words) + b"\n"
            return change
        for position, value in ((0, b"WRONG"), (1, b"2"), (2, b"highorder"),
                                (3, b"f" * 64), (4, b"f" * 64), (5, b"unexpected"),
                                (6, b"3"), (6, b"-1"), (6, b"1.0"), (6, b"true"),
                                (9, b"nan"), (9, b"inf"), (9, b"1e999"),
                                (9, b"1e-999"), (9, b"1_0"), (9, b"1" * 129)):
            with self.subTest(position=position, value=value):
                self.corrupt_response(replace(position, value))
        for mutation in (lambda b: b + b"extra\n", lambda b: b.rsplit(None, 1)[0],
                         lambda b: b"", lambda b: b"\xff", lambda b: b + b"\x00",
                         lambda b: b" " * (16 * 1024 * 1024 + 1)):
            with self.subTest(mutation=mutation):
                self.corrupt_response(mutation)

    def test_malformed_response_term_identity_and_origin(self):
        def change_term(mutate):
            def change(data):
                words = data.split()
                count_pos = 7 + 6 * int(words[6])
                first = count_pos + 1
                second = first + 7 + int(words[first])
                mutate(words, count_pos, first, second)
                return b" ".join(words) + b"\n"
            return change
        mutations = (
            lambda w, c, a, b: w.__setitem__(c, b"4097"),
            lambda w, c, a, b: w.__setitem__(a, b"0"),
            lambda w, c, a, b: w.__setitem__(a, b"12"),
            lambda w, c, a, b: w.__setitem__(a + 1, b"0"),
            lambda w, c, a, b: w.__setitem__(a + 1, b"3"),
            lambda w, c, a, b: w.__setitem__(a + 1, b"-2147483648"),
            lambda w, c, a, b: w.__setitem__(a + 2, b"0"),
            lambda w, c, a, b: w.__setitem__(a + 2, b"2147483648"),
            lambda w, c, a, b: w.__setitem__(a + 3, b"3"),
            lambda w, c, a, b: w.__setitem__(a + 4, b"0"),
            lambda w, c, a, b: w.__setitem__(a + 5, b"0"),
            lambda w, c, a, b: w.__setitem__(a + 5, b"18446744073709551616"),
            lambda w, c, a, b: w.__setitem__(a + 6, b"nan"),
            lambda w, c, a, b: w.__setitem__(slice(b, b + 8), w[a:a + 8]),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                self.corrupt_response(change_term(mutation))

    def test_malformed_finite_operating_points_and_reduced_inputs(self):
        def point_method():
            return self.native.amplifier_operating_point(
                .001, power_gain_db=10., output_p1db_dbm=20., output_saturation_dbm=23.,
                input_ip2_dbm=30., input_ip3_dbm=20.)

        def replace(position, value):
            def change(data):
                words = data.split()
                words[position] = value
                return b" ".join(words)
            return change

        for position in (6, 7, 8):
            with self.subTest(point_field=position):
                self.corrupt_response(replace(position, b"-1"), point_method)
        self.corrupt_response(lambda data: b" ".join(data.split()[:6] + [b"0", b"0"]))

        # Change a valid input identity to another valid key while retaining all
        # term fields; the response cannot silently substitute reduced inputs.
        self.corrupt_response(replace(10, b"8"))

        def change_generated_bandwidth(data):
            words = data.split()
            at = 8 + 6 * int(words[6])
            while int(words[at]) == 1:
                at += 7 + int(words[at])
            words[at + 1 + int(words[at]) + 2] = b"99"
            return b" ".join(words)
        self.corrupt_response(change_generated_bandwidth)

    def test_highorder_cannot_omit_obligatory_direct_terms(self):
        def highorder_method():
            return self.native.highorder_amplifier(
                1e6, self.sources, [], power_gain_db=10., output_p1db_dbm=20.,
                output_saturation_dbm=23.)

        def omit_direct_terms(data):
            words = data.split()
            count_position = 13 + 6 * int(words[12])
            return b" ".join(words[:count_position] + [b"0"])

        self.corrupt_response(omit_direct_terms, highorder_method)

    def test_polynomial_cannot_omit_required_nonzero_direct_terms(self):
        def direct_method():
            return self.native.coherent_polynomial(1e6, self.sources, [0., 1.])

        def omit_direct_terms(data):
            words = data.split()
            count_position = 7 + 6 * int(words[6])
            return b" ".join(words[:count_position] + [b"0"])

        self.corrupt_response(omit_direct_terms, direct_method)

    def test_polynomial_zero_and_underflowed_direct_terms_remain_valid(self):
        source = self.sources[0]
        cancelled = self.native.coherent_polynomial(
            1e6, [source, source._replace(amplitude=-source.amplitude)], [0., 1.])
        self.assertEqual(len(cancelled.inputs), 1)
        self.assertEqual(cancelled.inputs[0].amplitude, 0j)
        self.assertEqual(cancelled.terms, ())
        underflowed = self.native.coherent_polynomial(
            1e6, [source._replace(amplitude=1e-100 + 0j)], [0., 1e-300])
        self.assertEqual(len(underflowed.inputs), 1)
        self.assertNotEqual(underflowed.inputs[0].amplitude, 0j)
        self.assertEqual(underflowed.terms, ())

    def test_response_cannot_invent_zero_coefficient_orders(self):
        for operation, coefficient_position in (("polynomial", 10), ("highorder", 11)):
            def invented_order(data):
                words = data.split()
                self.assertEqual(words[coefficient_position], b"0.0")
                # Ask the real kernel for a structurally valid but different
                # coefficient response, preserving the original echoed identity.
                words[coefficient_position] = b"0.5"
                process = run_probe(b" ".join(words))
                self.assertEqual(process.returncode, 0)
                return process.returncode, process.stdout, process.stderr

            with self.subTest(operation=operation), mock.patch.object(
                    self.native, "_run", side_effect=invented_order):
                with self.assertRaises(NativeReplayError):
                    if operation == "polynomial":
                        self.native.coherent_polynomial(1e6, self.sources, [0., 1., 0.])
                    else:
                        self.native.highorder_amplifier(
                            1e6, self.sources, [0.], power_gain_db=10., output_p1db_dbm=20.,
                            output_saturation_dbm=23.)

    def test_process_nonzero_timeout_and_error_propagation(self):
        self.corrupt_response(lambda data: data, returncode=9)
        self.native = NativeReplay(PROBE)
        with self.assertRaises(NativeReplayError) as caught:
            self.native.coherent_polynomial(1e6, self.sources, [0., 0., 1.],
                                           reserved_group_max=MAX_UINT64)
        self.assertEqual(caught.exception.status, "numeric_range")
        self.assertEqual(self.native.exchanges, [], "failed calls cannot become evidence")

    def test_actual_process_pipe_limits_and_failures(self):
        # This auxiliary test binary supplies real process faults; no shell or
        # executable script association is needed on Windows.
        for mode, status in (("stdout_limit", "resource_limit"),
                             ("stderr_limit", "resource_limit"), ("timeout", "timeout"),
                             ("nonzero", "execution_failure")):
            with self.subTest(mode=mode), mock.patch.dict(
                    os.environ, {"RFMODEL_REFERENCE_ORACLE_FAULT": mode}):
                provider = NativeReplay(ORACLE, timeout=.02 if mode == "timeout" else 10.,
                                        manifest_path=Path(str(PROBE) + ".manifest.json"))
                with self.assertRaises(NativeReplayError) as caught:
                    provider.polynomial_coefficients_from_intercepts(10., [])
                self.assertEqual(caught.exception.status, status, str(caught.exception))
                self.assertEqual(provider.exchanges, [])
        with mock.patch("native_replay.subprocess.Popen", side_effect=OSError("test spawn fault")):
            with self.assertRaises(NativeReplayError):
                self.native.polynomial_coefficients_from_intercepts(10., [])

    def test_open_pipe_detects_excess_without_waiting_for_eof(self):
        # Deterministically model a writer that has produced its payload but
        # keeps the pipe open. Buffered read() asks for another raw read before
        # returning a short block; read1() must expose the excess immediately.
        class HeldPipe(io.RawIOBase):
            def __init__(self, process, payload):
                self.process, self.payload = process, bytearray(payload)

            def readable(self):
                return True

            def readinto(self, destination):
                if self.payload:
                    count = min(len(destination), len(self.payload))
                    destination[:count] = self.payload[:count]
                    del self.payload[:count]
                    return count
                self.process.needs_eof = True
                self.process.progress.set()
                if not self.process.killed.wait(2):
                    raise OSError("test did not terminate held pipe")
                return 0

        class HeldProcess:
            def __init__(self, stream, count):
                self.progress, self.killed = threading.Event(), threading.Event()
                self.needs_eof, self.deadline_reached = False, False
                held = io.BufferedReader(HeldPipe(self, b"x" * count))
                self.stdout = held if stream == "stdout" else io.BytesIO()
                self.stderr = held if stream == "stderr" else io.BytesIO()
                self.returncode = -9

            def kill(self):
                self.killed.set()
                self.progress.set()

            def wait(self, timeout=None):
                if timeout is not None:
                    if not self.progress.wait(2):
                        raise AssertionError("reader did not reach either overflow or held EOF")
                    if not self.killed.is_set():
                        self.deadline_reached = True
                        raise subprocess.TimeoutExpired("held test process", timeout)
                return self.returncode

        # Small injected budgets keep scheduling tests independent of throughput;
        # the real process test above still exercises the unchanged 16MiB/64KiB.
        for stream in ("stdout", "stderr"):
            for count, expected in ((64, "timeout"), (65, "resource_limit")):
                process = HeldProcess(stream, count)
                with self.subTest(stream=stream, count=count), mock.patch(
                        "native_replay.MAX_RESPONSE_BYTES", 64), mock.patch(
                        "native_replay.MAX_REQUEST_BYTES", 64), mock.patch(
                        "native_replay.subprocess.Popen", return_value=process):
                    with self.assertRaises(NativeReplayError) as caught:
                        self.native._run(b"")
                    self.assertEqual(caught.exception.status, expected)
                    self.assertEqual(process.deadline_reached, count == 64)
                    self.assertTrue(process.killed.is_set())
                    self.assertTrue(process.stdout.closed and process.stderr.closed)
                    self.assertEqual(self.native.exchanges, [])

    def test_observed_overflow_wins_simultaneous_process_deadline(self):
        # Force both schedules: excess observed before the racing wait result,
        # and excess drained only after timeout cleanup unblocks the pipe.
        class DelayedPipe(io.BytesIO):
            def __init__(self, process):
                super().__init__(b"x" * 65)
                self.process = process

            def read1(self, count):
                if self.process.after_deadline and not self.process.killed.wait(2):
                    raise OSError("test cleanup did not unblock reader")
                return super().read1(count)

        class RacedProcess:
            def __init__(self, after_deadline):
                self.after_deadline = after_deadline
                self.killed = threading.Event()
                self.stdout = DelayedPipe(self)
                self.stderr = io.BytesIO()
                self.returncode = -9

            def kill(self):
                self.killed.set()

            def wait(self, timeout=None):
                if timeout is not None:
                    if not self.after_deadline and not self.killed.wait(2):
                        raise AssertionError("reader must observe overflow before deadline")
                    raise subprocess.TimeoutExpired("raced test process", timeout)
                return self.returncode

        for after_deadline in (False, True):
            process = RacedProcess(after_deadline)
            with self.subTest(after_deadline=after_deadline), mock.patch(
                    "native_replay.MAX_RESPONSE_BYTES", 64), mock.patch(
                    "native_replay.subprocess.Popen", return_value=process):
                with self.assertRaises(NativeReplayError) as caught:
                    self.native._run(b"")
                self.assertEqual(caught.exception.status, "resource_limit")
                self.assertTrue(process.stdout.closed and process.stderr.closed)
                self.assertEqual(self.native.exchanges, [])

    def test_pipe_io_failure_is_not_claimed_as_observed_excess(self):
        class BrokenPipe(io.BytesIO):
            def read1(self, count):
                raise OSError("controlled pipe read fault")

        class BrokenProcess:
            def __init__(self, timed_out):
                self.stdout, self.stderr = BrokenPipe(), io.BytesIO()
                self.returncode, self.timed_out = 0, timed_out

            def kill(self):
                self.returncode = -9

            def wait(self, timeout=None):
                if timeout is not None and self.timed_out:
                    raise subprocess.TimeoutExpired("broken test process", timeout)
                return self.returncode

        for timed_out, expected in ((False, "execution_failure"), (True, "timeout")):
            process = BrokenProcess(timed_out)
            with self.subTest(timed_out=timed_out), mock.patch(
                    "native_replay.subprocess.Popen", return_value=process):
                with self.assertRaises(NativeReplayError) as caught:
                    self.native._run(b"")
                self.assertEqual(caught.exception.status, expected)
                self.assertTrue(process.stdout.closed and process.stderr.closed)
                self.assertEqual(self.native.exchanges, [])

    def test_sidecar_identity_required_and_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.json"
            with self.assertRaises((NativeReplayError, FileNotFoundError, ValueError)):
                NativeReplay(PROBE, manifest_path=manifest)
            for data in ({}, {"protocol": 2, "build_id": "1" * 64},
                         {"protocol": 1, "build_id": "wrong"}):
                manifest.write_text(json.dumps(data))
                with self.subTest(data=data), self.assertRaises((NativeReplayError, ValueError)):
                    NativeReplay(PROBE, manifest_path=manifest)
            actual = json.loads(Path(str(PROBE) + ".manifest.json").read_text())
            actual["build_id"] = "f" * 64
            manifest.write_text(json.dumps(actual))
            with self.assertRaises((NativeReplayError, ValueError)):
                wrong = NativeReplay(PROBE, manifest_path=manifest)
                wrong.polynomial_coefficients_from_intercepts(10., [])

    def test_sidecar_metadata_cannot_change_under_unchanged_build_identity(self):
        raw = Path(str(PROBE) + ".manifest.json").read_bytes()
        original = json.loads(raw)
        source_file, source_digest = next(iter(original["source_files"].items()))
        replacements = (
            ("compiler", '"version": ' + json.dumps(original["compiler"]["version"]),
             '"version": "tampered-compiler"'),
            ("revision", '"source_revision": ' + json.dumps(original["source_revision"]),
             '"source_revision": "tampered-revision"'),
            ("source_file", json.dumps(source_file) + ": " + json.dumps(source_digest),
             json.dumps(source_file) + ': "' + "f" * 64 + '"'),
        )
        with tempfile.TemporaryDirectory(prefix="rfmodel-manifest-tamper-") as tmp:
            manifest = Path(tmp) / "manifest.json"
            for name, before, after in replacements:
                with self.subTest(field=name):
                    self.assertIn(before.encode(), raw)
                    damaged = raw.replace(before.encode(), after.encode(), 1)
                    self.assertNotEqual(damaged, raw)
                    self.assertEqual(json.loads(damaged)["build_id"], original["build_id"])
                    # Preserve CMake's exact envelope/formatting. A rejection
                    # here must involve the body digest, not JSON reformatting.
                    manifest.write_bytes(damaged)
                    with self.assertRaisesRegex(NativeReplayError, "content.*build ID"):
                        NativeReplay(PROBE, manifest_path=manifest)

            # Rehashing an altered envelope cannot change the ID compiled into
            # the executable: a syntactically valid new manifest still fails the
            # actual request/response identity handshake.
            prefix = b'{\n  "build_id": "' + original["build_id"].encode() + b'",\n'
            new_id = hashlib.sha256(damaged[len(prefix):-3]).hexdigest()
            manifest.write_bytes(damaged.replace(original["build_id"].encode(), new_id.encode(), 1))
            changed = NativeReplay(PROBE, manifest_path=manifest)
            with self.assertRaises(NativeReplayError):
                changed.polynomial_coefficients_from_intercepts(10., [])

    def test_exchange_and_file_identity_digests_are_reproducible(self):
        first = self.invoke_polynomial()
        second = self.invoke_polynomial()
        self.assertEqual(first, second)
        self.assertEqual(len(self.native.exchanges), 2)
        self.assertEqual(self.native.exchanges[0], self.native.exchanges[1])
        manifest = self.native.manifest(archive_paths=[ROOT / "validation" / ARCHIVES[0]],
                                       scorer_paths=[Path(__file__)])
        encoded = json.dumps(manifest, sort_keys=True)
        for path in (PROBE, ROOT / "validation" / ARCHIVES[0], Path(__file__)):
            self.assertIn(hashlib.sha256(path.read_bytes()).hexdigest(), encoded)
        self.assertNotIn('"library_sha256"', json.dumps(self.native.execution_identity))

    def test_atomic_report_preserves_archives_existing_and_protected_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            protected = root / "archive.json"
            protected.write_bytes(b"historical bytes\n")
            original = protected.read_bytes()
            for destination in (protected, root / ".." / root.name / protected.name):
                with self.assertRaises((ValueError, FileExistsError, NativeReplayError)):
                    atomic_write_report(destination, {"changed": True}, protected_paths=[protected])
                self.assertEqual(protected.read_bytes(), original)
            report = root / "new-report.json"
            atomic_write_report(report, {"passed": True}, protected_paths=[protected, PROBE])
            self.assertEqual(json.loads(report.read_text()), {"passed": True})
            before = report.read_bytes()
            with self.assertRaises(FileExistsError):
                atomic_write_report(report, {"passed": False})
            self.assertEqual(report.read_bytes(), before)
            invalid = root / "invalid-report.json"
            with self.assertRaises((ValueError, NativeReplayError)):
                atomic_write_report(invalid, {"value": math.nan})
            self.assertFalse(invalid.exists())
            self.assertEqual(set(root.iterdir()), {protected, report})


class ArchiveManifestTests(unittest.TestCase):
    def test_all_eight_archived_files_retain_frozen_byte_identity(self):
        self.assertEqual(len(ARCHIVE_SHA256), 8)
        for name, expected in ARCHIVE_SHA256.items():
            with self.subTest(archive=name):
                actual = hashlib.sha256((ROOT / "validation" / name).read_bytes()).hexdigest()
                self.assertEqual(actual, expected)

    def run_manifest_cli(self, probe, report, *, environment=None):
        return subprocess.run(
            [sys.executable, "-I", "-S", str(Path(__file__).with_name("native_replay.py")),
             "--probe", str(probe), "--report", str(report)],
            capture_output=True, text=True, timeout=60, check=False, env=environment)

    def test_manifest_cli_preserves_frozen_evidence_and_operation_identity(self):
        with tempfile.TemporaryDirectory(prefix="rfmodel-native-manifest-") as tmp:
            report = Path(tmp) / "replay.json"
            process = self.run_manifest_cli(PROBE, report)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            manifest = json.loads(report.read_text())
            self.assertEqual(manifest["classification"], "diagnostic_only")
            self.assertIs(manifest["eligible_for_compatibility"], False)
            actual_archives = {Path(name).name: digest for name, digest in
                               manifest["available_archive_bytes_sha256"].items()}
            self.assertEqual(actual_archives, ARCHIVE_SHA256)
            self.assertEqual(manifest["native_replay_execution"]["executable_sha256"],
                             hashlib.sha256(PROBE.read_bytes()).hexdigest())
            self.assertEqual({row["operation"] for row in manifest["exchanges"]},
                             {"intercepts", "operating_point", "polynomial", "highorder"})
            self.assertEqual(manifest["unified_model_test"]["asserted_measured_input_holdout_count"], 1413)
            self.assertIs(manifest["unified_model_test"]["passed"], True)
            nominal = manifest["reports"]["nominal_source"]
            self.assertEqual([row["consistent"] for row in nominal["even_rules"].values()],
                             [True, True, False, False])
            self.assertEqual(nominal["relative_wave_tolerance"], 1e-7)

    def test_failed_manifest_execution_cannot_publish_or_replace_report(self):
        with tempfile.TemporaryDirectory(prefix="rfmodel-native-failure-") as tmp:
            root = Path(tmp)
            broken_probe = root / ORACLE.name
            shutil.copy2(ORACLE, broken_probe)
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"),
                            Path(str(broken_probe) + ".manifest.json"))
            environment = dict(os.environ, RFMODEL_REFERENCE_ORACLE_FAULT="nonzero")
            report = root / "replay.json"
            process = self.run_manifest_cli(broken_probe, report, environment=environment)
            self.assertNotEqual(process.returncode, 0)
            self.assertFalse(report.exists())
            existing = b"existing report must survive an execution failure\n"
            report.write_bytes(existing)
            process = self.run_manifest_cli(broken_probe, report, environment=environment)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(report.read_bytes(), existing)
            self.assertFalse(list(root.glob(".native-replay-*")))


class ImportIsolationTests(unittest.TestCase):
    def test_archived_suites_without_product_tree_or_imports(self):
        """Each suite gets a fresh -I -S interpreter with an active import ban."""
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

root, suite, probe = map(pathlib.Path, sys.argv[1:4])
mode = sys.argv[4]
assert mode in ("native", "pure")
assert not (root / "python").exists()
sys.path.insert(0, str(root / "scripts" / "reference"))
sys.meta_path.insert(0, RejectProduct())
sys.argv = [str(suite)] + (["--native-probe", str(probe)] if mode == "native" else [])
try:
    runpy.run_path(str(suite), run_name="__main__")
except SystemExit as exc:
    if exc.code not in (0, None):
        raise
assert not any(k == "rfmodel" or k.startswith("rfmodel.") for k in sys.modules)
print("ACTIVE_IMPORT_BAN_PASSED", suite.name)
'''
        with tempfile.TemporaryDirectory(prefix="rfmodel-no-product-") as tmp:
            root = Path(tmp)
            scripts = root / "scripts" / "reference"
            scripts.mkdir(parents=True)
            for source in (ROOT / "scripts" / "reference").glob("*.py"):
                shutil.copyfile(source, scripts / source.name)
            data = root / "validation"
            data.mkdir()
            for name in dict.fromkeys(ARCHIVES + (
                    "am-pm-existing-reference-evidence-20261012.json",
                    "am-pm-point-derivative-plan-20261013.json",
                    "am-pm-parameter-study-observations-20261013.json",
                    "am-pm-parameter-study-manifest-20261013.json",
                    "am-pm-parameter-study-capture-sources-20261013.json",
                    "am-pm-point-derivative-adapter-plan-20261013.json",
                    "am-pm-point-derivative-adapter-amendment-20261013.json",
                    "am-pm-adapter-observations-20261013.json",
                    "am-pm-adapter-captures-20261013.json") + DATAPOINTS_ZERO_FIXTURES
                    + DATAPOINTS_ZERO_RESULT_FIXTURES + DATAPOINTS_RAW_AXIS_RESULT_FIXTURES):
                shutil.copyfile(ROOT / "validation" / name, data / name)
            probe = root / PROBE.name
            shutil.copy2(PROBE, probe)
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"), Path(str(probe) + ".manifest.json"))
            for suite in SUITES + PURE_PYTHON_SUITES:
                with self.subTest(suite=suite):
                    process = subprocess.run(
                        [sys.executable, "-I", "-S", "-c", driver, str(root),
                         str(scripts / suite), str(probe), "native" if suite in SUITES else "pure"],
                        cwd=root, capture_output=True, text=True, timeout=180, check=False)
                    self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                    self.assertIn("ACTIVE_IMPORT_BAN_PASSED " + suite, process.stdout)


if __name__ == "__main__":
    unittest.main()
