"""Chebyshev bindings, independent pole oracle and JSON network integration."""

import cmath
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(ROOT / "python"))
import rfmodel
from rfmodel import Library, RFModelError
from rfmodel.model_file import analyze, load


def network(model):
    return {
        "format": "rfmodel.linear-network",
        "version": 1,
        "frequencies_hz": [0, 1e8, 7e8, 1e9, 2e9, 1e10],
        "devices": [{"id": "d", "model": model}],
        "external_ports": [["d", 0], ["d", 1]],
    }


class ChebyshevTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_four_responses_against_analog_poles(self):
        for response in ("lowpass", "highpass", "bandpass", "bandstop"):
            edges = (
                {"passband_hz": 1e9}
                if response in ("lowpass", "highpass")
                else {"lower_passband_hz": 1e9, "upper_passband_hz": 4e9}
            )
            for order in (2, 3, 8, 16, 64):
                for ripple in (0.01, 0.1, 1, 10):
                    epsilon = math.sqrt(math.expm1(ripple * math.log(10) / 10))
                    mu = math.asinh(1 / epsilon) / order
                    for frequency in (0.1e9, 0.7e9, 1e9, 1.5e9, 3e9, 4e9, 10e9):
                        f = frequency / 1e9
                        mapped = {
                            "lowpass": f,
                            "highpass": -1 / f,
                            "bandpass": (f * f - 4) / (3 * f),
                            "bandstop": -3 * f / (f * f - 4),
                        }[response]
                        expected = 1 if order % 2 else 10 ** (-ripple / 20)
                        for k in range(order):
                            theta = (2 * k + 1) * math.pi / (2 * order)
                            pole = complex(
                                -math.sinh(mu) * math.sin(theta), math.cosh(mu) * math.cos(theta)
                            )
                            expected *= -pole / (1j * mapped - pole)
                        for topology in ("open", "short"):
                            s = self.library.chebyshev_filter(
                                frequency,
                                response=response,
                                order=order,
                                ripple_db=ripple,
                                input_stopband=topology,
                                **edges,
                            )
                            self.assertLess(abs(s[1][0] - expected), 3e-12)
                            self.assertEqual(s[1][0], s[0][1])
                            self.assertAlmostEqual(
                                abs(s[0][0]) ** 2 + abs(s[1][0]) ** 2, 1, places=12
                            )
                            self.assertLess(
                                abs(s[0][0] * s[0][1].conjugate() + s[1][0] * s[1][1].conjugate()),
                                1e-14,
                            )

    def test_independent_edge_attenuation_and_parity(self):
        for response in ("lowpass", "highpass", "bandpass", "bandstop"):
            band = response in ("bandpass", "bandstop")
            edges = (
                {"lower_passband_hz": 1e9, "upper_passband_hz": 4e9}
                if band
                else {"passband_hz": 1e9}
            )
            for order in (2, 3, 8, 9):
                for attenuation in (0.1, 1, 10):
                    for f in ((1e9, 4e9) if band else (1e9,)):
                        s = self.library.chebyshev_filter(
                            f,
                            response=response,
                            order=order,
                            ripple_db=0.1,
                            passband_attenuation_db=attenuation,
                            **edges,
                        )
                        self.assertAlmostEqual(
                            abs(s[1][0]) ** 2, 10 ** (-attenuation / 10), places=11
                        )
                f = 2e9 if response == "bandpass" else 0
                if response in ("lowpass", "bandpass", "bandstop"):
                    s = self.library.chebyshev_filter(
                        f, response=response, order=order, ripple_db=1, **edges
                    )
                    self.assertAlmostEqual(
                        abs(s[1][0]) ** 2, 1 if order % 2 else 10 ** (-0.1), places=12
                    )

    def test_dual_topology_preserves_transmission(self):
        for order in (2, 3, 8):
            for frequency in (0, 0.7e9, 1e9, 2e9, 1e308):
                common = dict(response="lowpass", order=order, passband_hz=1e9)
                a = self.library.chebyshev_filter(frequency, input_stopband="open", **common)
                b = self.library.chebyshev_filter(frequency, input_stopband="short", **common)
                self.assertAlmostEqual(a[1][0], b[1][0])
                self.assertAlmostEqual(a[0][0], -b[0][0])
                self.assertAlmostEqual(a[1][1], -b[1][1])

    def test_json_against_explicit_lc_network(self):
        document = network(
            {"type": "chebyshev_lossless", "response": "lowpass", "order": 3, "passband_hz": 1e9}
        )
        actual = analyze(self.library, document)
        physical = copy.deepcopy(document)
        omega = 2 * math.pi * 1e9
        gamma = math.sinh(math.asinh(1 / math.sqrt(10**0.01 - 1)) / 3)
        g1 = 1 / gamma
        g2 = 2 / ((gamma * gamma + 0.75) * g1)
        physical["devices"] = [
            {
                "id": "l1",
                "model": {
                    "type": "inductor",
                    "connection": "series",
                    "inductance_h": g1 * 50 / omega,
                },
            },
            {
                "id": "c",
                "model": {
                    "type": "capacitor",
                    "connection": "shunt",
                    "capacitance_f": g2 / (50 * omega),
                },
            },
            {
                "id": "l2",
                "model": {
                    "type": "inductor",
                    "connection": "series",
                    "inductance_h": g1 * 50 / omega,
                },
            },
        ]
        physical["connections"] = [[["l1", 1], ["c", 0]], [["c", 1], ["l2", 0]]]
        physical["external_ports"] = [["l1", 0], ["l2", 1]]
        expected = analyze(self.library, physical)
        for a, b in zip(actual["samples"], expected["samples"]):
            for row in range(2):
                for column in range(2):
                    self.assertLess(
                        abs(complex(*a["s"][row][column]) - complex(*b["s"][row][column])), 1e-12
                    )

    def test_bandpass_example_and_lossless_noise(self):
        document = load(ROOT / "examples/chebyshev-bandpass.json")
        result = analyze(self.library, document)
        for sample in result["samples"]:
            noise = sample["noise_w_per_hz"]
            self.assertLess(max(abs(complex(*v)) for row in noise for v in row), 1e-32)
            f = sample["frequency_hz"]
            if f == 0:
                expected = 0
            else:
                x = (f * f - 90e6 * 100e6) / (10e6 * f)
                expected = 1 / (1 + (10**0.01 - 1) * (4 * x**3 - 3 * x) ** 2)
            self.assertAlmostEqual(abs(complex(*sample["s"][1][0])) ** 2, expected, places=11)

    def test_narrow_band_and_extreme_attenuation(self):
        for low in (1e-200, 1e200):
            high = math.nextafter(low, math.inf)
            for response in ("bandpass", "bandstop"):
                for f in (low, high):
                    s = self.library.chebyshev_filter(
                        f, response=response, order=3, lower_passband_hz=low, upper_passband_hz=high
                    )
                    self.assertAlmostEqual(abs(s[1][0]) ** 2, 10 ** (-0.01), places=12)
        for ripple in (math.ulp(0.0), 1e-12, 100, 6000):
            for order in (2, 3, 64):
                for f in (0, 1e-300, 1e9, 1e308):
                    s = self.library.chebyshev_filter(
                        f, response="lowpass", order=order, passband_hz=1e9, ripple_db=ripple
                    )
                    self.assertTrue(
                        all(
                            math.isfinite(v.real) and math.isfinite(v.imag)
                            for row in s
                            for v in row
                        )
                    )

    def test_odd_polynomial_near_dc_without_cancellation(self):
        # T3(x)=4*x**3-3*x and epsilon is approximately 1e300.
        s = self.library.chebyshev_filter(
            1e-291, response="lowpass", order=3, passband_hz=1e9, ripple_db=6000
        )
        self.assertAlmostEqual(abs(s[1][0]) ** 2, 0.1, places=12)
        # x=1e-608 underflows, but epsilon*T3(x) is representable.
        s = self.library.chebyshev_filter(
            1e-308, response="lowpass", order=3, passband_hz=1e300, ripple_db=6000
        )
        self.assertLess(abs(abs(s[0][0]) / 3e-308 - 1), 2e-12)

    def test_native_validation(self):
        valid = dict(response="lowpass", order=3, passband_hz=1e9)
        for change in (
            {"order": True},
            {"order": 3.0},
            {"order": 1},
            {"order": 65},
            {"passband_hz": 0},
            {"passband_hz": float("inf")},
            {"passband_attenuation_db": 0},
            {"ripple_db": 0},
            {"ripple_db": 1e308},
            {"ripple_db": 1, "passband_attenuation_db": 0.1},
            {"passband_attenuation_db": float("nan")},
            {"upper_passband_hz": 2e9},
            {"input_stopband": "invalid"},
            {"reference_ohms": -50},
            {"response": "invalid"},
        ):
            with (
                self.subTest(change=change),
                self.assertRaises((ValueError, TypeError, RFModelError)),
            ):
                self.library.chebyshev_filter(1e9, **dict(valid, **change))
        for f in (-1, float("inf"), float("nan")):
            with self.assertRaises(RFModelError):
                self.library.chebyshev_filter(f, **valid)

    def test_json_rejects_unimplemented_vendor_fields(self):
        valid = {
            "type": "chebyshev_lossless",
            "response": "lowpass",
            "order": 3,
            "passband_hz": 1e9,
        }
        for change in (
            {"IL": 1},
            {"Amax": 100},
            {"order": True},
            {"passband_hz": True},
            {"upper_passband_hz": 2e9},
            {"order": 1},
        ):
            with self.subTest(change=change), self.assertRaises(ValueError):
                analyze(self.library, network(dict(valid, **change)))

    def test_cli_failure_preserves_output(self):
        document = load(ROOT / "examples/chebyshev-bandpass.json")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        with tempfile.TemporaryDirectory() as directory:
            model, output = Path(directory) / "model.json", Path(directory) / "result.json"
            model.write_text(json.dumps(document), encoding="utf-8")
            command = [
                sys.executable,
                "-m",
                "rfmodel",
                str(model),
                "--library",
                LIBRARY_PATH,
                "--output",
                str(output),
            ]
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_bytes()
            document["devices"][0]["model"]["order"] = 1
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()
