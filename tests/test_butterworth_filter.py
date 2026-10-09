"""Butterworth bindings, independent pole oracle and JSON network integration."""

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


class ButterworthTests(unittest.TestCase):
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
                for attenuation in (0.1, 1, 3.010299956639812, 20):
                    scale = math.expm1(attenuation * math.log(10) / 10) ** (1 / (2 * order))
                    for frequency in (0.1e9, 0.7e9, 1e9, 1.5e9, 3e9, 4e9, 10e9):
                        f = frequency / 1e9
                        mapped = {
                            "lowpass": f,
                            "highpass": -1 / f,
                            "bandpass": (f * f - 4) / (3 * f),
                            "bandstop": -3 * f / (f * f - 4),
                        }[response] * scale
                        expected = 1 + 0j
                        for k in range(order):
                            pole = cmath.exp(1j * math.pi * (2 * k + 1 + order) / (2 * order))
                            expected /= 1j * mapped - pole
                        for topology in ("open", "short"):
                            s = self.library.butterworth_filter(
                                frequency,
                                response=response,
                                order=order,
                                passband_attenuation_db=attenuation,
                                input_stopband=topology,
                                **edges,
                            )
                            self.assertLess(abs(s[1][0] - expected), 3e-12)
                            self.assertEqual(s[1][0], s[0][1])
                            self.assertAlmostEqual(
                                abs(s[0][0]) ** 2 + abs(s[1][0]) ** 2, 1, places=11
                            )

    def test_dual_topology_preserves_transmission(self):
        for order in (2, 3, 8):
            for frequency in (0, 0.7e9, 1e9, 2e9, 1e308):
                common = dict(response="lowpass", order=order, passband_hz=1e9)
                a = self.library.butterworth_filter(frequency, input_stopband="open", **common)
                b = self.library.butterworth_filter(frequency, input_stopband="short", **common)
                self.assertAlmostEqual(a[1][0], b[1][0])
                self.assertAlmostEqual(a[0][0], -b[0][0])
                self.assertAlmostEqual(a[1][1], -b[1][1])

    def test_json_against_explicit_lc_network(self):
        document = network(
            {"type": "butterworth_ladder", "response": "lowpass", "order": 3, "passband_hz": 1e9}
        )
        actual = analyze(self.library, document)
        physical = copy.deepcopy(document)
        omega = 2 * math.pi * 1e9
        physical["devices"] = [
            {
                "id": "l1",
                "model": {"type": "inductor", "connection": "series", "inductance_h": 50 / omega},
            },
            {
                "id": "c",
                "model": {
                    "type": "capacitor",
                    "connection": "shunt",
                    "capacitance_f": 2 / (50 * omega),
                },
            },
            {
                "id": "l2",
                "model": {"type": "inductor", "connection": "series", "inductance_h": 50 / omega},
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
        document = load(ROOT / "examples/butterworth-bandpass.json")
        result = analyze(self.library, document)
        for sample in result["samples"]:
            noise = sample["noise_w_per_hz"]
            self.assertLess(max(abs(complex(*v)) for row in noise for v in row), 1e-32)
            f = sample["frequency_hz"]
            expected = 0 if f == 0 else 1 / (1 + ((f * f - 90e6 * 100e6) / (10e6 * f)) ** 6)
            self.assertAlmostEqual(abs(complex(*sample["s"][1][0])) ** 2, expected, places=11)

    def test_narrow_band_and_extreme_attenuation(self):
        for low in (1e-200, 1e200):
            high = math.nextafter(low, math.inf)
            for response in ("bandpass", "bandstop"):
                for f in (low, high):
                    s = self.library.butterworth_filter(
                        f, response=response, order=3, lower_passband_hz=low, upper_passband_hz=high
                    )
                    self.assertAlmostEqual(abs(s[1][0]) ** 2, 0.5, places=12)
        for attenuation in (math.ulp(0.0), 1e308):
            for f in (0, 1e-300, 1e9, 1e308):
                s = self.library.butterworth_filter(
                    f,
                    response="lowpass",
                    order=64,
                    passband_hz=1e9,
                    passband_attenuation_db=attenuation,
                )
                self.assertTrue(
                    all(math.isfinite(v.real) and math.isfinite(v.imag) for row in s for v in row)
                )

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
                self.library.butterworth_filter(1e9, **dict(valid, **change))
        for f in (-1, float("inf"), float("nan")):
            with self.assertRaises(RFModelError):
                self.library.butterworth_filter(f, **valid)

    def test_json_rejects_unimplemented_vendor_fields(self):
        valid = {
            "type": "butterworth_ladder",
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
        document = load(ROOT / "examples/butterworth-bandpass.json")
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
