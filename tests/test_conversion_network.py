"""Multi-device signal, correlated noise, explicit boundary and CLI regression."""

import cmath
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
from rfmodel.conversion_network_file import analyze_conversion_network
from rfmodel.model_file import load


class ConversionNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def example(self):
        return load(ROOT / "examples/filtered-conversion-network.json")

    def analyze(self, document, directory=None):
        return analyze_conversion_network(self.library, document, base_directory=directory)

    def channel(self, result, name, port, index):
        return next(
            i
            for i, c in enumerate(result["channels"])
            if (c["device"], c["port"], c["bin"]) == (name, port, index)
        )

    def wave(self, result, name, port, index):
        return complex(*result["channels"][self.channel(result, name, port, index)]["outgoing"])

    def warm_pads(self):
        document = self.example()
        for device, gain in ((document["devices"][0], 0.5), (document["devices"][2], 0.25)):
            device["model"] = {"type": "linear", "s": [[0, gain], [gain, 0]]}
        return document

    def test_filtered_chain_against_independent_poles(self):
        result = self.analyze(self.example())

        def butterworth(x):
            s = 1j * x
            return 1 / (s**3 + 2 * s**2 + 2 * s + 1)

        epsilon = math.sqrt(10 ** (0.1 / 10) - 1)
        mu = math.asinh(1 / epsilon) / 3
        poles = [
            -math.sinh(mu) * math.sin((2 * k - 1) * math.pi / 6)
            + 1j * math.cosh(mu) * math.cos((2 * k - 1) * math.pi / 6)
            for k in range(1, 4)
        ]

        def chebyshev(x):
            return math.prod([-p for p in poles]) / math.prod([1j * x - p for p in poles])

        for index, rf in ((2, 1.2), (22, 1.2)):
            expected = butterworth(rf) * chebyshev(index / 3)
            self.assertAlmostEqual(self.wave(result, "if_filter", 1, index), expected)
        self.assertLess(abs(self.wave(result, "if_filter", 1, 18)), 1e-14)
        i = self.channel(result, "if_filter", 1, 2)
        expected = (abs(butterworth(0.8)) ** 2 + abs(butterworth(1.2)) ** 2) * abs(
            chebyshev(2 / 3)
        ) ** 2
        self.assertAlmostEqual(
            complex(*result["noise_covariance_w_per_hz"][i][i]).real / 1e-20, expected
        )
        self.assertLess(result["relative_residual"], 1e-12)

    def test_warm_pad_chain_preserves_c_and_p(self):
        result = self.analyze(self.warm_pads())
        self.assertAlmostEqual(self.wave(result, "if_filter", 1, 2), 0.125)
        i, j, k = [self.channel(result, "if_filter", 1, b) for b in (2, 18, 22)]
        c = result["noise_covariance_w_per_hz"]
        p = result["noise_complementary_w_per_hz"]
        thermal = 1.380649e-23 * 290
        # Two independent RF bands fold into IF; each pad also emits thermal noise.
        self.assertAlmostEqual(complex(*c[i][i]).real / 1e-20, 0.03125 + 1.03125 * thermal / 1e-20)
        cross = 0.0625 * (0.25e-20 + 0.75 * thermal)
        self.assertAlmostEqual(complex(*c[i][k]) / 1e-20, cross / 1e-20)
        self.assertAlmostEqual(complex(*p[i][j]) / 1e-20, cross / 1e-20)
        self.assertLess(abs(complex(*c[i][j])), 1e-32)

    def test_wires_match_bins_not_channel_positions(self):
        document = self.warm_pads()
        expected = self.analyze(document)
        document["devices"][1]["channels"].reverse()
        result = self.analyze(document)
        for index in (2, 18, 22):
            self.assertAlmostEqual(
                self.wave(result, "if_filter", 1, index), self.wave(expected, "if_filter", 1, index)
            )
        # Incident waves at each physical wire equal the opposite outgoing waves.
        for bin_index in (8, 12):
            i = self.channel(result, "mixer", 0, bin_index)
            self.assertAlmostEqual(
                complex(*result["channels"][i]["incident"]),
                self.wave(result, "rf_filter", 1, bin_index),
            )

    def test_dc_lift_has_real_noise(self):
        document = {
            "format": "rfmodel.conversion-network",
            "version": 1,
            "spacing_hz": 1,
            "devices": [
                {
                    "id": "load",
                    "bins": [0],
                    "model": {"type": "linear", "s": [[0]]},
                    "noise": {"temperature_k": 290},
                }
            ],
            "boundaries": [{"channel": ["load", 0, 0], "noise_w_per_hz": 2e-20}],
        }
        result = self.analyze(document)
        c = complex(*result["noise_covariance_w_per_hz"][0][0])
        p = complex(*result["noise_complementary_w_per_hz"][0][0])
        self.assertAlmostEqual(c.real / (1.380649e-23 * 290), 1)
        self.assertEqual(c, p)
        self.assertEqual(c.imag, 0)

    def test_frequency_mismatch_is_rejected(self):
        document = self.example()
        document["devices"][0]["bins"][0] = 7
        document["boundaries"][0]["channel"][2] = 7
        with self.assertRaises(RFModelError):
            self.analyze(document)

    def test_repeated_and_self_connections_rejected(self):
        for wire in ([["rf_filter", 1], ["mixer", 0]], [["mixer", 0], ["mixer", 0]]):
            document = self.example()
            document["connections"].append(wire)
            with self.assertRaises(RFModelError):
                self.analyze(document)

    def test_boundary_coverage_is_explicit(self):
        document = self.example()
        document["boundaries"].pop()
        with self.assertRaisesRegex(ValueError, "Every unconnected"):
            self.analyze(document)
        document = self.example()
        document["boundaries"].append({"channel": ["mixer", 0, 8]})
        with self.assertRaisesRegex(ValueError, "connected boundary"):
            self.analyze(document)

    def test_connected_source_noise_rejected(self):
        document = self.example()
        matrix = [[0 for _ in range(5)] for _ in range(5)]
        matrix[0][0] = 1e-20
        document["devices"][1]["source_noise"] = {"covariance": matrix}
        with self.assertRaises(RFModelError):
            self.analyze(document)

    def test_conflicting_source_noise_rejected(self):
        document = self.example()
        document["devices"][0]["source_noise"] = {"noiseless": True}
        with self.assertRaisesRegex(ValueError, "Choose device source_noise"):
            self.analyze(document)

    def test_direct_library_feedback(self):
        devices = [
            {
                "channels": [(0, 1), (1, 1)],
                "direct": [[0, 0.5], [0.5, 0]],
                "source": [1, 0],
                "reflection": [0.2, 0],
            },
            {
                "channels": [(0, 1)],
                "direct": [[0.4]],
                "intrinsic_covariance": [[2]],
                "intrinsic_complementary": [[0.6 + 0.2j]],
            },
        ]
        result = self.library.conversion_network(1, devices, [((0, 1), (1, 0))])
        # b_out=.5*(1+.2*.5*.4*b_out): infinite reflected round trips.
        self.assertAlmostEqual(result.outgoing[1], 0.5 / (1 - 0.02))
        self.assertAlmostEqual(result.incident[2], result.outgoing[1])
        self.assertAlmostEqual(result.incident[1], result.outgoing[2])
        gains = [0.5 / 0.98, 0.05 / 0.98, 1 / 0.98]
        for i in range(3):
            for j in range(3):
                self.assertAlmostEqual(result.noise_covariance[i][j], 2 * gains[i] * gains[j])
                self.assertAlmostEqual(
                    result.noise_complementary[i][j], (0.6 + 0.2j) * gains[i] * gains[j]
                )

    def test_invalid_schema_and_dimensions(self):
        for field, value in (
            ("version", True),
            ("unknown", 1),
            ("spacing_hz", 0),
            ("reference_ohms", float("nan")),
        ):
            with self.assertRaises((ValueError, RFModelError)):
                self.analyze(dict(self.example(), **{field: value}))
        for devices in (
            [],
            [{"channels": [(0, 1)], "direct": [[1, 0], [0, 1]]}],
            [{"channels": [(0, 1)], "direct": [[1]], "unknown": 1}],
        ):
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                self.library.conversion_network(1, devices)
        document = self.example()
        del document["devices"][1]["noise"]
        with self.assertRaises(ValueError):
            self.analyze(document)

    def cli(self, model, output):
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        return subprocess.run(
            [
                sys.executable,
                "-m",
                "rfmodel",
                str(model),
                "--library",
                LIBRARY_PATH,
                "--output",
                str(output),
            ],
            env=environment,
            capture_output=True,
            text=True,
        )

    def test_nested_touchstone_and_cli_input_protection(self):
        document = self.warm_pads()
        document["devices"][0]["model"] = {
            "type": "linear",
            "device": {"type": "touchstone", "path": "pad.s2p"},
        }
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            touchstone = directory / "pad.s2p"
            touchstone.write_text(
                "# Hz S RI R 50\n800000000 0 0 .5 0 .5 0 0 0\n1200000000 0 0 .5 0 .5 0 0 0\n",
                encoding="utf-8",
            )
            original = touchstone.read_bytes()
            result = self.analyze(document, directory)
            self.assertAlmostEqual(self.wave(result, "if_filter", 1, 2), 0.125)
            model = directory / "model.json"
            model.write_text(json.dumps(document), encoding="utf-8")
            run = self.cli(model, touchstone)
            self.assertEqual(run.returncode, 1)
            self.assertIn("Touchstone input", run.stderr)
            self.assertEqual(touchstone.read_bytes(), original)

    def test_cli_failure_preserves_previous_result(self):
        document = self.example()
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.json"
            output = Path(directory) / "output.json"
            model.write_text(json.dumps(document), encoding="utf-8")
            run = self.cli(model, output)
            self.assertEqual(run.returncode, 0, run.stderr)
            original = output.read_bytes()
            document["boundaries"].pop()
            model.write_text(json.dumps(document), encoding="utf-8")
            run = self.cli(model, output)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
