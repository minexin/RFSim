"""Reference-temperature conversion NF with explicit reference and thermal bands."""

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
from rfmodel.conversion_network_file import analyze_conversion_network
from rfmodel.model_file import load

KT = 1.380649e-23 * 290


class ConversionNoiseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def mixer(self):
        channels = [(0, 8), (0, 12), (1, 2), (1, 18), (1, 22)]
        a, b = self.library.ideal_mixer_conversion(1e8, channels, lo_bin=10)
        return {"channels": channels, "direct": a, "conjugate": b}

    def metric(self, devices, reference=(1,), thermal=(0, 1), output=2, **options):
        return self.library.conversion_noise_analysis(
            1e8,
            devices,
            reference_channels=reference,
            thermal_channels=thermal,
            output_channel=output,
            **options,
        )

    def test_ideal_mixer_single_and_double_band(self):
        devices = [self.mixer()]
        ssb = self.metric(devices)
        dsb = self.metric(devices, reference=(0, 1))
        self.assertAlmostEqual(ssb.reference_gain, 1)
        self.assertAlmostEqual(ssb.output_noise_w_per_hz / KT, 2)
        self.assertAlmostEqual(ssb.noise_factor, 2)
        self.assertAlmostEqual(dsb.noise_factor, 1)
        self.assertAlmostEqual(ssb.noise_figure_db - dsb.noise_figure_db, 10 * math.log10(2))
        self.assertAlmostEqual(ssb.equivalent_input_temperature_k, 290)

    def test_image_suppression_is_not_a_fixed_three_db_correction(self):
        device = self.mixer()
        device["conjugate"] = [list(row) for row in device["conjugate"]]
        device["conjugate"][2][0] = 0.1
        metric = self.metric([device])
        self.assertAlmostEqual(metric.noise_factor, 1.01)
        self.assertAlmostEqual(self.metric([device], thermal=(1,)).noise_factor, 1)
        self.assertAlmostEqual(self.metric([device], reference=(0, 1)).noise_factor, 1)

    def test_phase_sensitive_gain_is_quadrature_average(self):
        device = {
            "channels": [(0, 1), (1, 2)],
            "direct": [[0, 0], [2, 0]],
            "conjugate": [[0, 0], [1, 0]],
        }
        metric = self.metric([device], reference=(0,), thermal=(0,), output=1)
        self.assertAlmostEqual(metric.reference_gain, 5)
        self.assertAlmostEqual(metric.noise_factor, 1)
        # A real coherent input has gain 9; a quadrature input has gain 1.
        for source, gain in ((1, 9), (1j, 1)):
            device["source"] = [source, 0]
            wave = self.library.conversion_network(1e8, [device]).outgoing[1]
            self.assertAlmostEqual(abs(wave) ** 2, gain)

    def test_unheated_lossless_termination_is_allowed(self):
        device = self.mixer()
        device["reflection"] = [0, 0, 0, 1, -1]
        self.assertAlmostEqual(self.metric([device]).noise_factor, 2)

    def test_warm_pad_mixer_chain_analytic(self):
        document = load(ROOT / "examples/conversion-noise-figure.json")
        result = analyze_conversion_network(self.library, document)
        ssb, dsb = result["noise_analyses"]
        self.assertAlmostEqual(ssb["reference_gain"], 0.015625)
        self.assertAlmostEqual(ssb["output_noise_w_per_hz"] / KT, 1.0625)
        self.assertAlmostEqual(ssb["noise_factor"], 68)
        self.assertAlmostEqual(dsb["noise_factor"], 34)
        self.assertAlmostEqual(ssb["reference_output_noise_w_per_hz"] / KT, 0.015625)

    def test_passive_pad_reference_temperature(self):
        device = {
            "channels": [(0, 1), (1, 1)],
            "direct": [[0, 0.5], [0.5, 0]],
            "intrinsic_covariance": [[0.75 * KT, 0], [0, 0.75 * KT]],
        }
        for temperature in (145, 290, 580):
            metric = self.metric(
                [device],
                reference=(0,),
                thermal=(0,),
                output=1,
                reference_temperature_k=temperature,
            )
            self.assertAlmostEqual(metric.noise_factor, 1 + 3 * 290 / temperature)
            self.assertAlmostEqual(metric.equivalent_input_temperature_k, 870)

    def test_complex_reflections_match_closed_form(self):
        gamma = 0.2 + 0.3j
        load = -0.1 + 0.25j
        a = 0.1
        b = 0.7
        d = -0.2
        # Passive correlated noise: kT*(I-S*S^H).
        c00 = 1 - a * a - b * b
        c11 = 1 - d * d - b * b
        c01 = -b * (a + d)
        device = {
            "channels": [(0, 1), (1, 1)],
            "direct": [[a, b], [b, d]],
            "reflection": [gamma, load],
            "intrinsic_covariance": [[KT * c00, KT * c01], [KT * c01, KT * c11]],
        }
        metric = self.metric([device], reference=(0,), thermal=(0,), output=1)
        h = (1 - a * gamma) / b
        added = (
            abs(gamma) ** 2 * c00 + abs(h) ** 2 * c11 + 2 * (gamma * h.conjugate() * c01).real
        ) / (1 - abs(gamma) ** 2)
        denominator = (1 - a * gamma) * (1 - d * load) - b * b * gamma * load
        gain = (1 - abs(gamma) ** 2) * (1 - abs(load) ** 2) * b * b / abs(denominator) ** 2
        self.assertAlmostEqual(metric.reference_gain, gain)
        self.assertAlmostEqual(metric.noise_factor, 1 + added)

    def test_reference_experiment_does_not_mutate_sources(self):
        device = self.mixer()
        device["source"] = [0, 2 + 3j, 0, 0, 0]
        c = [[0 for _ in range(5)] for _ in range(5)]
        c[0][0] = 9e-20
        device["source_covariance"] = c
        original = copy.deepcopy(device)
        before = self.library.conversion_network(1e8, [device])
        self.assertAlmostEqual(self.metric([device]).noise_factor, 2)
        after = self.library.conversion_network(1e8, [device])
        self.assertEqual(before, after)
        self.assertEqual(device, original)

    def test_invalid_channel_sets(self):
        cases = [
            {"reference": ()},
            {"thermal": ()},
            {"reference": (1, 1)},
            {"thermal": (0, 1, 1)},
            {"reference": (3,)},
            {"output": 0},
            {"output": 8},
            {"reference": (True,)},
        ]
        for options in cases:
            with (
                self.subTest(options=options),
                self.assertRaises((ValueError, TypeError, RFModelError)),
            ):
                self.metric([self.mixer()], **options)
        for temperature in (0, -1, math.nan, math.inf):
            with self.assertRaises(RFModelError):
                self.metric([self.mixer()], reference_temperature_k=temperature)

    def test_dc_zero_gain_and_active_load_rejected(self):
        devices = [
            {"channels": [(0, 0), (1, 1)], "direct": [[0, 0], [1, 0]]},
            {"channels": [(0, 1), (1, 1)], "direct": [[0, 0], [0, 0]]},
            {"channels": [(0, 1), (1, 1)], "direct": [[0, 0], [1, 0]], "reflection": [0, 1]},
        ]
        for device in devices:
            with self.assertRaises(RFModelError):
                self.metric([device], reference=(0,), thermal=(0,), output=1)

    def test_json_strict_fields_and_channel_names(self):
        base = load(ROOT / "examples/conversion-noise-figure.json")
        for edit in (
            lambda a: a.update(extra=1),
            lambda a: a.update(reference_channels=[["missing", 0, 1]]),
            lambda a: a.update(output_channel=["if_pad", True, 2]),
            lambda a: a.update(name=""),
        ):
            document = copy.deepcopy(base)
            edit(document["noise_analyses"][0])
            with self.assertRaises(ValueError):
                analyze_conversion_network(self.library, document)
        document = copy.deepcopy(base)
        document["noise_analyses"][1]["name"] = "ssb"
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.library, document)

    def test_cli_failure_preserves_result(self):
        document = load(ROOT / "examples/conversion-noise-figure.json")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.json"
            output = Path(directory) / "output.json"
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
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            original = output.read_bytes()
            document["noise_analyses"][0]["thermal_channels"] = []
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
