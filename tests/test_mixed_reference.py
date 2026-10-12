"""Exercise mixed-reference C/Python/JSON networks with installed-mode support."""
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
from rfmodel.model_file import analyze, _encode, _matrix, load
from rfmodel.spectrum_file import analyze_spectrum

ROOT = Path(__file__).resolve().parents[1]
KT = 1.380649e-23 * 290


def resistors():
    return {"format": "rfmodel.linear-network", "version": 1,
            "frequencies_hz": [1e9, 2e9], "temperature_k": 290,
            "devices": [
                {"id": "a", "model": {"type": "resistor", "connection": "series", "resistance_ohms": 25},
                 "reference_samples_ohms": [[50, [25, 20]], [50, [90, -15]]]},
                {"id": "b", "model": {"type": "resistor", "connection": "series", "resistance_ohms": 75},
                 "reference_impedances_ohms": [[100, -10], 50]}],
            "connections": [[["a", 1], ["b", 0]]],
            "external_ports": [["a", 0], ["b", 1]],
            "signal_boundaries": [{"port": ["a", 0], "source": [.3, .1]},
                                  {"port": ["b", 1], "source": [.05, 0], "reflection": [.2, .1]}],
            "noise_boundaries": [{"port": ["a", 0], "reflection": 0, "temperature_k": 290},
                                 {"port": ["b", 1], "reflection": [.2, .1], "temperature_k": 170}]}


class MixedReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = rfmodel.Library(LIBRARY_PATH)

    def matrix_near(self, first, second, scale=1.):
        self.assertEqual(len(first), len(second))
        for a, b in zip(first, second):
            self.assertEqual(len(a), len(b))
            for x, y in zip(a, b):
                self.assertLess(abs(x-y)/scale, 2e-11)

    def test_native_binding_query_and_wiring(self):
        with self.lib.network() as n:
            self.assertEqual(n.reference_impedances_ohms, ())
            self.assertEqual(n.add_power_waves([[0, 1], [1, 0]], [50, 25]), 0)
            self.assertEqual(n.add_power_waves([[0, 1], [1, 0]], [100, 50]), 2)
            self.assertEqual(n.reference_impedances_ohms, (50, 25, 100, 50))
            n.connect(1, 2)
            self.matrix_near(n.external_s([0, 3]), [[.6, .8], [.8, -.6]])
            n.terminate(0, source=1)
            n.terminate(3)
            waves = n.solve()
            self.assertAlmostEqual(waves.outgoing[3], .8)
            self.assertAlmostEqual(abs(waves.outgoing[0])**2 + abs(waves.outgoing[3])**2, 1)

    def test_json_models_signal_and_thermal_noise(self):
        doc = resistors()
        original = copy.deepcopy(doc)
        plain = copy.deepcopy(doc)
        for d in plain["devices"]:
            d.pop("reference_impedances_ohms", None)
            d.pop("reference_samples_ohms", None)
        result, baseline = analyze(self.lib, doc), analyze(self.lib, plain)
        self.assertEqual(doc, original)
        for sample, base in zip(result["samples"], baseline["samples"]):
            self.matrix_near(_matrix(sample["s"]), [[.5, .5], [.5, .5]])
            self.matrix_near(_matrix(sample["s"]), _matrix(base["s"]))
            self.matrix_near(_matrix(sample["noise_w_per_hz"]), _matrix(base["noise_w_per_hz"]), KT)
            for key in ("incident_w_per_hz", "outgoing_w_per_hz"):
                self.matrix_near(_matrix(sample["loaded_noise"][key]), _matrix(base["loaded_noise"][key]), KT)
            ports, original_ports = sample["signal"]["ports"], base["signal"]["ports"]
            for i in (0, 3):
                for key in ("incident", "outgoing"):
                    self.assertAlmostEqual(complex(*ports[i][key]), complex(*original_ports[i][key]))
            vi = []
            for i in (1, 2):
                port = ports[i]
                z = complex(*port["reference_impedance_ohms"])
                a, b = complex(*port["incident"]), complex(*port["outgoing"])
                v, current = (z.conjugate()*a+z*b)/math.sqrt(z.real), (a-b)/math.sqrt(z.real)
                self.assertAlmostEqual((v*current.conjugate()).real, port["net_into_device_w"])
                vi.append((v, current))
            self.assertAlmostEqual(vi[0][0], vi[1][0])
            self.assertAlmostEqual(vi[0][1], -vi[1][1])
            self.assertEqual(sample["port_impedances_ohms"], [[50, 0], [50, 0]])
            self.assertEqual(sample["loaded_noise"]["port_impedances_ohms"], [[50, 0], [50, 0]])

    def test_explicit_s_is_already_in_declared_coordinates(self):
        doc = {"format": "rfmodel.linear-network", "version": 1, "frequencies_hz": [0],
               "devices": [{"id": "a", "s": [[0, 1], [1, 0]], "reference_impedances_ohms": [50, 25]},
                           {"id": "b", "s": [[0, 1], [1, 0]], "reference_impedances_ohms": [100, 50]}],
               "connections": [[["a", 1], ["b", 0]]], "external_ports": [["a", 0], ["b", 1]]}
        self.matrix_near(_matrix(analyze(self.lib, doc)["samples"][0]["s"]), [[.6, .8], [.8, -.6]])

    def test_cross_device_covariance_is_local_and_correlations_survive(self):
        common_s = [[.1, .4, 0, 0], [.7, -.15, 0, 0], [0, 0, -.2, .3], [0, 0, .6, .08]]
        vector = [1e-5, 2e-5j, -1e-5+1e-5j, 3e-5]
        noise = [[x*complex(y).conjugate() for y in vector] for x in vector]
        references = [31+12j, 73-21j, 19+8j, 110+31j]
        changed = self.lib.renormalize_power_waves(common_s, [50]*4, references, noise=noise)
        def document(matrix, covariance, refs):
            return {"format": "rfmodel.linear-network", "version": 1, "frequencies_hz": [1e9],
                    "devices": [{"id": name, "s": _encode([row[start:start+2] for row in matrix[start:start+2]]),
                                 "reference_impedances_ohms": [[z.real, z.imag] for z in refs[start:start+2]]}
                                for name, start in (("a", 0), ("b", 2))],
                    "connections": [[["a", 1], ["b", 0]]], "external_ports": [["a", 0], ["b", 1]],
                    "intrinsic_noise_samples": [_encode(covariance)], "output_reference_impedances_ohms": [50, 50]}
        baseline = analyze(self.lib, document(common_s, noise, [50+0j]*4))["samples"][0]
        result = analyze(self.lib, document(changed.scattering, changed.noise_correlation, references))["samples"][0]
        self.matrix_near(_matrix(result["s"]), _matrix(baseline["s"]))
        self.matrix_near(_matrix(result["noise_w_per_hz"]), _matrix(baseline["noise_w_per_hz"]), 1e-9)

    def test_explicit_device_noise_stays_in_local_coordinates(self):
        covariance = [[2e-9, 1e-9j], [-1e-9j, 3e-9]]
        doc = {"format": "rfmodel.linear-network", "version": 1, "frequencies_hz": [1e9],
               "devices": [{"id": "a", "model": {"type": "matched_transmission", "loss_db": 6},
                            "reference_impedances_ohms": [[25, 10], [100, -20]],
                            "noise": {"covariance": _encode(covariance)}}],
               "external_ports": [["a", 0], ["a", 1]]}
        result = analyze(self.lib, doc)["samples"][0]
        self.matrix_near(_matrix(result["noise_w_per_hz"]), covariance, 1e-9)

    def test_touchstone_embedded_noise_and_nf_transform_together(self):
        base = {"format": "rfmodel.linear-network", "version": 1, "frequencies_hz": [1e9, 2e9],
                "devices": [{"id": "a", "model": {"type": "touchstone", "path": str(ROOT / "examples/noisy-amplifier.s2p")},
                             "noise": {"touchstone": True}}],
                "external_ports": [["a", 0], ["a", 1]], "noise_analysis": {}}
        mixed = copy.deepcopy(base)
        mixed["devices"][0]["reference_samples_ohms"] = [[[25, 10], [100, -20]], [[75, -5], [30, 12]]]
        mixed["output_reference_impedances_ohms"] = [[40, 15], [90, -8]]
        base["output_reference_impedances_ohms"] = mixed["output_reference_impedances_ohms"]
        for a, b in zip(analyze(self.lib, mixed)["samples"], analyze(self.lib, base)["samples"]):
            self.matrix_near(_matrix(a["s"]), _matrix(b["s"]))
            self.matrix_near(_matrix(a["noise_w_per_hz"]), _matrix(b["noise_w_per_hz"]), KT)
            self.assertAlmostEqual(a["noise_analysis"]["noise_figure_db"], b["noise_analysis"]["noise_figure_db"])
            self.assertEqual(a["noise_analysis"]["source_impedance_ohms"], [50, 0])

    def test_output_conversion_preserves_boundary_coordinates(self):
        doc = resistors()
        doc["devices"][0]["reference_samples_ohms"] = [[[31, 12], [25, 20]], [[45, -10], [90, -15]]]
        doc["devices"][1]["reference_impedances_ohms"] = [[100, -10], [110, 31]]
        baseline = analyze(self.lib, doc)
        doc["output_reference_impedances_ohms"] = [50, 50]
        for changed, before in zip(analyze(self.lib, doc)["samples"], baseline["samples"]):
            self.assertEqual(changed["signal"], before["signal"])
            self.assertEqual(changed["loaded_noise"], before["loaded_noise"])
            self.assertEqual(changed["port_impedances_ohms"], [[50, 0], [50, 0]])
            self.assertNotIn("reference_ohms", changed["signal"])
            self.assertNotIn("reference_ohms", changed["loaded_noise"])
            oldrefs = [complex(*z) for z in before["port_impedances_ohms"]]
            expected = self.lib.renormalize_power_waves(_matrix(before["s"]), oldrefs, [50, 50],
                                                       noise=_matrix(before["noise_w_per_hz"]))
            self.matrix_near(_matrix(changed["s"]), expected.scattering)
            self.matrix_near(_matrix(changed["noise_w_per_hz"]), expected.noise_correlation, KT)

    def test_internal_termination_uses_its_local_reference(self):
        zload, zport = 100+15j, 73-21j
        mixed_gamma = (zload-zport)/(zload+zport.conjugate())
        common_gamma = (zload-50)/(zload+50)
        base = {"format": "rfmodel.linear-network", "version": 1, "frequencies_hz": [1e9],
                "devices": [{"id": "a", "model": {"type": "matched_transmission", "loss_db": 6}}],
                "external_ports": [["a", 0]], "temperature_k": 290,
                "terminations": [{"port": ["a", 1], "reflection": [common_gamma.real, common_gamma.imag]}]}
        mixed = copy.deepcopy(base)
        mixed["devices"][0]["reference_impedances_ohms"] = [[31, 12], [73, -21]]
        mixed["terminations"][0]["reflection"] = [mixed_gamma.real, mixed_gamma.imag]
        mixed["output_reference_impedances_ohms"] = [50]
        a, b = analyze(self.lib, mixed)["samples"][0], analyze(self.lib, base)["samples"][0]
        self.matrix_near(_matrix(a["s"]), _matrix(b["s"]))
        self.matrix_near(_matrix(a["noise_w_per_hz"]), _matrix(b["noise_w_per_hz"]), KT)

    def test_invalid_json_references(self):
        for changes in (
            {"reference_impedances_ohms": None}, {"reference_impedances_ohms": [50]},
            {"reference_impedances_ohms": [50, 0]}, {"reference_impedances_ohms": [50, [-2, 1]]},
            {"reference_impedances_ohms": [50, True]}, {"reference_impedances_ohms": [50, [50, float('nan')]]},
            {"reference_samples_ohms": None}, {"reference_samples_ohms": [[50, 50]]},
            {"reference_samples_ohms": [[50, 50], [50]]},
            {"reference_samples_ohms": [[50, 50], [50, 50]], "reference_impedances_ohms": [50, 50]},
        ):
            doc = resistors()
            doc["devices"][0].pop("reference_samples_ohms")
            doc["devices"][0].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                analyze(self.lib, doc)

    def test_python_failed_add_and_closed_handles(self):
        with self.lib.network() as n:
            for refs in ([], [50], [50, 0], [50, -1], [True, 50], [50, complex(50, float('inf'))]):
                with self.subTest(refs=refs), self.assertRaises((ValueError, rfmodel.RFModelError)):
                    n.add_power_waves([[0, 1], [1, 0]], refs)
                self.assertEqual(n.port_count, 0)
                self.assertEqual(n.reference_impedances_ohms, ())
            n.add_power_waves([[0]], [31+12j])
            self.assertEqual(n.add([[0]]), 1)
            self.assertEqual(n.reference_impedances_ohms, (31+12j, 50))
        with self.assertRaises(RuntimeError):
            n.add_power_waves([[0]], [50])
        with self.assertRaises(RuntimeError):
            _ = n.reference_impedances_ohms

    def test_uniform_reference_algorithms_reject_mixed_coordinates(self):
        with self.lib.network() as n:
            n.add_power_waves([[0, .5], [.5, 0]], [31+12j, 50])
            with self.assertRaisesRegex(rfmodel.RFModelError, "independent power-wave references"):
                n.transmit_spectrum(1e6, {1: .1}, [0, 1])
        network = {"devices": [{"id": "a", "s": [[0, .5], [.5, 0]],
                               "reference_impedances_ohms": [25, 75]}],
                   "external_ports": [["a", 0], ["a", 1]]}
        doc = {"format": "rfmodel.spectrum-chain", "version": 1, "spacing_hz": 1e6,
               "input": [{"bin": 1, "amplitude": .1}],
               "stages": [{"type": "linear", "network": network}]}
        with self.assertRaises(ValueError):
            analyze_spectrum(self.lib, doc)

    def test_cli_example_and_failure_preserve_output(self):
        with tempfile.TemporaryDirectory() as directory:
            model, output = Path(directory)/"input.json", Path(directory)/"result.json"
            model.write_text(json.dumps(resistors()), encoding="utf-8")
            env = dict(os.environ, PYTHONPATH=str(Path(rfmodel.__file__).resolve().parents[1]))
            command = [sys.executable, "-m", "rfmodel", str(model), "--library", LIBRARY_PATH, "--output", str(output)]
            run = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_bytes()
            result = json.loads(saved)
            self.assertEqual(result["samples"][1]["signal"]["ports"][1]["reference_impedance_ohms"], [90, -15])
            invalid = resistors()
            invalid["devices"][0]["reference_samples_ohms"][1][0] = 0
            model.write_text(json.dumps(invalid), encoding="utf-8")
            run = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)

    def test_example_is_rerunnable(self):
        result = analyze(self.lib, load(ROOT / "examples/mixed-reference-network.json"))
        self.assertEqual(len(result["samples"]), 2)
        self.assertEqual(result["samples"][0]["wave_definition"], "power")


if __name__ == "__main__":
    unittest.main()
