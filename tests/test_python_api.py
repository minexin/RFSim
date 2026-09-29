"""Exercise the Python binding against a real, explicitly supplied shared library."""
import gc
import copy
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
import weakref

LIBRARY_PATH = sys.argv.pop(1)
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from rfmodel import Library, RFModelError
import rfmodel
from rfmodel.model_file import analyze, load


class PythonApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_complex_cascade_and_waves(self):
        with self.library.network(75.) as network:
            self.assertEqual(network.add([[0, -0.5j], [-0.5j, 0]]), 0)
            self.assertEqual(network.add([[0, 0.2], [0.2, 0]]), 2)
            network.connect(1, 2)
            self.assertEqual(network.port_count, 4)
            self.assertAlmostEqual(network.external_s([0, 3])[1][0], -0.1j)
            network.terminate(0, source=2.)
            network.terminate(3)
            waves = network.solve()
            self.assertAlmostEqual(waves.outgoing[3], -0.2j)
            self.assertLess(waves.relative_residual, 1e-12)

    def test_native_error_preserves_network(self):
        with self.library.network() as network:
            with self.assertRaises(RFModelError) as caught:
                network.add([[0, 1], [1, 0]], reference_ohms=75.)
            self.assertEqual(caught.exception.status, 1)
            self.assertIn("reference", str(caught.exception))
            self.assertEqual(network.port_count, 0)
            network.add([[0, 1], [1, 0]])
            network.connect(0, 1)
            with self.assertRaises(RFModelError) as caught:
                network.solve()
            self.assertEqual(caught.exception.status, 2)

    def test_shape_indices_and_nonfinite(self):
        with self.library.network() as network:
            for matrix in ([], [[0, 1]], [[0], [1]]):
                with self.assertRaises(ValueError):
                    network.add(matrix)
            with self.assertRaises(RFModelError):
                network.add([[float("nan")]])
            network.add([[0, 1], [1, 0]])
            for index in (-1, 2**80, 0.5, True):
                with self.assertRaises((ValueError, TypeError)):
                    network.terminate(index)
            with self.assertRaises(RFModelError):
                network.external_s([0, 0])
            self.assertEqual(network.external_s([1, 0]), ((0j, 1+0j), (1+0j, 0j)))

    def test_context_exception_and_double_close(self):
        network = self.library.network()
        with self.assertRaisesRegex(ValueError, "user exception"):
            with network:
                raise ValueError("user exception")
        network.close()
        for action in (lambda: network.port_count, network.solve,
                       lambda: network.external_s([0]), lambda: network.add([[0]])):
            with self.assertRaisesRegex(RuntimeError, "closed"):
                action()

    def test_finalizer_does_not_retain_network(self):
        network = self.library.network()
        reference = weakref.ref(network)
        finalizer = network._finalizer
        del network
        gc.collect()
        self.assertIsNone(reference())
        self.assertFalse(finalizer.alive)

    def test_invalid_creation(self):
        with self.assertRaises(RFModelError):
            self.library.network(-1.)
        with self.assertRaises(FileNotFoundError):
            Library(Path(LIBRARY_PATH).parent / "missing-rfmodel-library")

    def test_cascaded_thermal_noise(self):
        thermal = 1.380649e-23 * 290.
        scattering = [[0, 0.5], [0.5, 0]]
        block = self.library.passive_noise(scattering)
        self.assertAlmostEqual(block[0][0] / thermal, 0.75)
        covariance = [[0j] * 4 for _ in range(4)]
        for offset in (0, 2):
            for row in range(2):
                for column in range(2):
                    covariance[offset + row][offset + column] = block[row][column]
        with self.library.network() as network:
            network.add(scattering)
            network.add(scattering)
            network.connect(1, 2)
            actual = network.external_noise([0, 3], covariance)
            self.assertAlmostEqual(actual[1][1] / thermal, 1. - 0.25**2)
            self.assertAlmostEqual(actual[0][0] / thermal, 1. - 0.25**2)
        with self.assertRaises(RFModelError):
            self.library.passive_noise([[2.]])

    def test_correlated_noise_and_validation(self):
        with self.library.network() as network:
            network.add([[0.]])
            network.add([[0.]])
            covariance = [[1e-20, 0.5e-20j], [-0.5e-20j, 1e-20]]
            result = network.external_noise([1, 0], covariance)
            self.assertAlmostEqual(result[0][1] / 1e-20, -0.5j)
            self.assertAlmostEqual(result[1][0] / 1e-20, 0.5j)
            for bad in ([[1., 2.], [2., 1.]], [[1., 1j], [1j, 1.]]):
                with self.assertRaises(RFModelError):
                    network.external_noise([0, 1], bad)
            with self.assertRaises(ValueError):
                network.external_noise([0], [[1.]])
        with self.assertRaisesRegex(RuntimeError, "closed"):
            network.external_noise([0], [[1.]])

    def test_model_file_sweep(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/linear-noise.json")
        result = analyze(self.library, document)
        thermal = 1.380649e-23 * 290.
        self.assertEqual(result["samples"][0]["s"][1][0], [0., -0.5])
        self.assertEqual(result["samples"][1]["s"][1][0], [-0.5, 0.])
        for sample in result["samples"]:
            self.assertAlmostEqual(sample["noise_w_per_hz"][1][1][0] / thermal, 0.75)

    def test_model_file_rejects_ambiguous_or_invalid_input(self):
        original = load(Path(__file__).resolve().parents[1] / "examples/linear-noise.json")
        for key, value in (("version", True), ("version", 2), ("unknown", 1),
                           ("frequencies_hz", [2., 1.]), ("temperature_k", None),
                           ("external_ports", [["pad", -1]]),
                           ("intrinsic_noise_samples", [])):
            document = copy.deepcopy(original)
            document[key] = value
            with self.assertRaises(ValueError):
                analyze(self.library, document)
        document = copy.deepcopy(original)
        document["devices"][1]["id"] = "pad"
        with self.assertRaises(ValueError):
            analyze(self.library, document)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            for text in ('{"version":1,"version":2}', '{"value":NaN}'):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    load(path)

    def test_model_file_correlated_noise_and_termination(self):
        document = {"format": "rfmodel.linear-network", "version": 1,
                    "frequencies_hz": [1e9],
                    "devices": [{"id": "a", "s": [[0, 1], [1, 0]]}],
                    "terminations": [{"port": ["a", 1], "reflection": 0.5}],
                    "external_ports": [["a", 0]],
                    "intrinsic_noise_samples": [[[1., [0., 0.5]], [[0., -0.5], 1.]]]}
        point = analyze(self.library, document)["samples"][0]
        self.assertEqual(point["s"], [[[0.5, 0.]]])
        self.assertAlmostEqual(point["noise_w_per_hz"][0][0][0], 1.25)

    def test_model_file_cli_preserves_output_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.json"
            output = Path(directory) / "result.json"
            model.write_text((Path(__file__).resolve().parents[1] /
                              "examples/linear-noise.json").read_text(encoding="utf-8"),
                             encoding="utf-8")
            environment = dict(os.environ)
            environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
            command = [sys.executable, "-m", "rfmodel", str(model), "--library",
                       str(Path(LIBRARY_PATH).resolve()), "--output", str(output)]
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_text(encoding="utf-8")
            self.assertEqual(len(json.loads(saved)["samples"]), 2)
            model.write_text('{"format":"invalid"}', encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_text(encoding="utf-8"), saved)


if __name__ == "__main__":
    unittest.main()
