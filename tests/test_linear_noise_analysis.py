"""Exercise linear JSON noise postprocessing using a real native library."""

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
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
import rfmodel
from rfmodel import Library
from rfmodel.model_file import analyze, load

ROOT = Path(__file__).resolve().parents[1]


class LinearNoiseAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_frequency_sources_and_complex_references(self):
        document = load(ROOT / "examples/linear-noise-analysis.json")
        result = analyze(self.library, document)
        for index, sample in enumerate(result["samples"]):
            noise = sample["noise_analysis"]
            source = (50 + 0j, 25 + 30j)[index]
            reference = complex(*document["output_reference_samples_ohms"][index][0])
            self.assertEqual(noise["source_impedance_ohms"], [source.real, source.imag])
            self.assertEqual(noise["reference_impedances_ohms"], sample["port_impedances_ohms"])
            self.assertEqual(noise["source_reflection_convention"], "a_over_b")
            self.assertEqual(noise["wave_definition"], "power")
            self.assertAlmostEqual(noise["minimum_noise_figure_db"], 10 * math.log10(4))
            self.assertAlmostEqual(noise["noise_resistance_ohms"], 46.875)
            self.assertAlmostEqual(
                complex(*noise["optimum_source_reflection"]),
                (50 - reference) / (50 + reference.conjugate()),
            )
            admittance = 1 / source
            expected = 4 + 46.875 / admittance.real * abs(admittance - 1 / 50) ** 2
            self.assertAlmostEqual(noise["noise_figure_db"], 10 * math.log10(expected))

    def test_default_source_is_physical_internal_reference(self):
        document = load(ROOT / "examples/linear-noise.json")
        baseline = analyze(self.library, document)
        self.assertNotIn("noise_analysis", baseline["samples"][0])
        document["noise_analysis"] = {}
        real = analyze(self.library, document)
        document["output_reference_impedances_ohms"] = [[25, 40], [100, -20]]
        changed = analyze(self.library, document)
        for first, second in zip(real["samples"], changed["samples"]):
            self.assertEqual(second["noise_analysis"]["source_impedance_ohms"], [50, 0])
            for key in ("noise_figure_db", "minimum_noise_figure_db", "noise_resistance_ohms"):
                self.assertAlmostEqual(first["noise_analysis"][key], second["noise_analysis"][key])
        for first, second in zip(baseline["samples"], real["samples"]):
            self.assertEqual(first["s"], second["s"])
            self.assertEqual(first["noise_w_per_hz"], second["noise_w_per_hz"])

    def test_reference_temperature_and_explicit_noiseless(self):
        document = load(ROOT / "examples/linear-noise.json")
        document["noise_analysis"] = {"reference_temperature_k": 580}
        sample = analyze(self.library, document)["samples"][0]
        self.assertAlmostEqual(sample["noise_analysis"]["noise_figure_db"], 10 * math.log10(2.5))
        self.assertAlmostEqual(sample["noise_analysis"]["noise_resistance_ohms"], 46.875 / 2)
        del document["temperature_k"]
        for device in document["devices"]:
            device["noise"] = {"noiseless": True}
        sample = analyze(self.library, document)["samples"][0]["noise_analysis"]
        self.assertEqual(sample["noise_figure_db"], 0)
        self.assertEqual(sample["minimum_noise_figure_db"], 0)
        self.assertEqual(sample["optimum_source_reflection"], [0, 0])

    def test_external_port_order_selects_direction(self):
        kt = 1.380649e-23 * 290
        document = {
            "format": "rfmodel.linear-network",
            "version": 1,
            "frequencies_hz": [1e9],
            "devices": [
                {
                    "id": "amp",
                    "s": [[0.1, 0.1], [2, 0.2]],
                    "noise": {"covariance": [[kt, 0], [0, 4 * kt]]},
                }
            ],
            "external_ports": [["amp", 0], ["amp", 1]],
            "noise_analysis": {},
        }
        forward = analyze(self.library, document)["samples"][0]["noise_analysis"]
        self.assertAlmostEqual(forward["noise_figure_db"], 10 * math.log10(2))
        document["external_ports"].reverse()
        reverse = analyze(self.library, document)["samples"][0]["noise_analysis"]
        self.assertAlmostEqual(reverse["noise_figure_db"], 10 * math.log10(101))

    def test_boundary_results_do_not_supply_intrinsic_noise(self):
        document = load(ROOT / "examples/loaded-noise.json")
        document["noise_analysis"] = {"source_impedance_ohms": [25, 30]}
        baseline = analyze(self.library, document)["samples"][0]
        document["noise_boundaries"][0]["temperature_k"] = 500
        document["signal_boundaries"] = [
            {"port": ["pad", 0], "source": 1, "reflection": 0.3},
            {"port": ["pad", 1], "reflection": 0.7},
        ]
        changed = analyze(self.library, document)["samples"][0]
        self.assertEqual(changed["noise_analysis"], baseline["noise_analysis"])
        self.assertNotEqual(changed["loaded_noise"], baseline["loaded_noise"])

    def test_touchstone_noise_analysis(self):
        document = load(ROOT / "examples/measured-noise.json")
        document["noise_analysis"] = {"source_impedance_ohms": 75}
        document["output_reference_impedances_ohms"] = [[25, 10], [100, -15]]
        result = analyze(self.library, document, base_directory=ROOT / "examples")
        for sample in result["samples"]:
            noise = sample["noise_analysis"]
            self.assertAlmostEqual(noise["minimum_noise_figure_db"], 10 * math.log10(2))
            self.assertAlmostEqual(noise["noise_figure_db"], 10 * math.log10(2))
            self.assertAlmostEqual(noise["noise_resistance_ohms"], 37.5)
            self.assertAlmostEqual(
                complex(*noise["optimum_source_reflection"]), (75 - (25 + 10j)) / (75 + (25 - 10j))
            )

    def test_invalid_requests(self):
        document = load(ROOT / "examples/linear-noise.json")
        invalid = [
            None,
            False,
            [],
            {"unknown": 0},
            {"reference_temperature_k": 0},
            {"reference_temperature_k": True},
            {"source_impedance_ohms": None},
            {"source_impedance_ohms": 0},
            {"source_impedance_ohms": [-1, 10]},
            {"source_impedance_ohms": [50, math.nan]},
            {"source_impedance_samples_ohms": None},
            {"source_impedance_samples_ohms": []},
            {"source_impedance_samples_ohms": [50]},
            {"source_impedance_ohms": 50, "source_impedance_samples_ohms": [50, 50]},
        ]
        for request in invalid:
            with self.subTest(request=request), self.assertRaises(ValueError):
                analyze(self.library, dict(document, noise_analysis=request))
        document["noise_analysis"] = {}
        missing = copy.deepcopy(document)
        del missing["temperature_k"]
        with self.assertRaisesRegex(ValueError, "explicit intrinsic noise"):
            analyze(self.library, missing)
        document["external_ports"] = document["external_ports"][:1]
        with self.assertRaisesRegex(ValueError, "two external ports"):
            analyze(self.library, document)

    def test_failure_identifies_frequency(self):
        document = load(ROOT / "examples/linear-noise.json")
        document["noise_analysis"] = {}
        document["devices"][1]["s_samples"][1] = [[0, 0], [0, 0]]
        with self.assertRaisesRegex(ValueError, "Noise analysis failed at 2e[+]09 Hz"):
            analyze(self.library, document)

    def test_cli_preserves_output_after_later_frequency_failure(self):
        document = load(ROOT / "examples/linear-noise-analysis.json")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        with tempfile.TemporaryDirectory() as directory:
            model, output = Path(directory) / "model.json", Path(directory) / "output.json"
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
            self.assertEqual(len(json.loads(saved)["samples"]), 2)
            document["devices"][1]["s_samples"][1] = [[0, 0], [0, 0]]
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1, run.stderr)
            self.assertIn("2e+09 Hz", run.stderr)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()
