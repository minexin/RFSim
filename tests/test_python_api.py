"""Exercise the Python binding against a real, explicitly supplied shared library."""
import gc
import copy
import json
import math
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
from rfmodel.spectrum_file import analyze_spectrum


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

    def test_cubic_two_tone_and_compression(self):
        tones = {10: math.sqrt(1e-5), 13: math.sqrt(1e-5)}
        result = self.library.cubic_amplifier(1e6, tones, power_gain_db=20., input_ip3_dbm=10.)
        self.assertAlmostEqual(abs(result[7])**2 / 1e-9, 1.)
        self.assertAlmostEqual(abs(result[16])**2 / 1e-9, 1.)
        power = (1. - 10**(-1./20.)) * 0.01
        result = self.library.cubic_amplifier(
            1e6, {10: math.sqrt(power)}, power_gain_db=20., input_ip3_dbm=10.)
        self.assertAlmostEqual(10*math.log10(abs(result[10])**2/power), 19.)
        self.assertEqual(self.library.cubic_amplifier(
            1e6, {}, power_gain_db=20., input_ip3_dbm=10.), {})

    def test_mixer_phase_dc_and_coherent_cancellation(self):
        result = self.library.ideal_mixer(1e6, {10: 1.}, lo_bin=2, lo_phase_radians=math.pi/2)
        self.assertAlmostEqual(result[8], -1j)
        self.assertAlmostEqual(result[12], 1j)
        result = self.library.ideal_mixer(1e6, {2: 1.}, lo_bin=2)
        self.assertAlmostEqual(result[0], math.sqrt(2.))
        self.assertAlmostEqual(result[4], 1.)
        result = self.library.ideal_mixer(1e6, {1: 1., 3: -1.}, lo_bin=2)
        self.assertNotIn(1, result)
        self.assertAlmostEqual(self.library.ideal_mixer(1e6, {1: 1j}, lo_bin=2,
                                                lo_phase_radians=math.pi/2)[1], 1.)

    def test_spectrum_invalid_inputs(self):
        for tones in ({-1: 1.}, {2**40: 1.}, {1.5: 1.}, {True: 1.}):
            with self.assertRaises((ValueError, TypeError)):
                self.library.ideal_mixer(1e6, tones, lo_bin=2)
        for spacing, tones, lo in ((0., {1: 1.}, 2), (1e6, {0: 1j}, 2),
                                   (1e6, {1: float("nan")}, 2), (1e6, {1: 1.}, 0)):
            with self.assertRaises(RFModelError):
                self.library.ideal_mixer(spacing, tones, lo_bin=lo)

    def test_spectrum_file_chain(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/two-tone-mixer.json")
        result = analyze_spectrum(self.library, document)
        amp = {item["bin"]: item for item in result["stages"][0]["spectrum"]}
        mixer = {item["bin"]: item for item in result["stages"][1]["spectrum"]}
        self.assertAlmostEqual(amp[7]["power_w"] / 1e-9, 1.)
        self.assertAlmostEqual(mixer[1]["power_w"] / 1e-9, 1.)
        self.assertAlmostEqual(mixer[2]["power_w"] / amp[10]["power_w"], 1.)
        self.assertEqual(mixer[1]["frequency_hz"], 1e6)
        for key, value in (("spacing_hz", 0), ("version", True), ("stages", [])):
            bad = copy.deepcopy(document)
            bad[key] = value
            with self.assertRaises(ValueError):
                analyze_spectrum(self.library, bad)
        for change in ("duplicate", "dc", "stage", "unknown"):
            bad = copy.deepcopy(document)
            if change == "duplicate":
                bad["input"].append(bad["input"][0])
            elif change == "dc":
                bad["input"] = [{"bin": 0, "amplitude": [0, 1]}]
            elif change == "stage":
                bad["stages"][1]["id"] = "amp"
            else:
                bad["stages"][0]["noise_figure"] = 3
            with self.assertRaises(ValueError):
                analyze_spectrum(self.library, bad)

    def test_spectrum_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "spectrum.json"
            model = Path(__file__).resolve().parents[1] / "examples/two-tone-mixer.json"
            environment = dict(os.environ)
            environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
            run = subprocess.run([sys.executable, "-m", "rfmodel", str(model), "--library",
                                  str(Path(LIBRARY_PATH).resolve()), "--output", str(output)],
                                 env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["format"], "rfmodel.spectrum-results")
            self.assertEqual([stage["id"] for stage in result["stages"]], ["amp", "mixer"])

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

    def test_loaded_noise_scalar_and_thermal_equilibrium(self):
        s, gamma = 0.2+0.3j, -0.1+0.2j
        result = self.library.loaded_noise([[s]], [[2.]], [gamma], [[3.]])
        denominator = abs(1-s*gamma)**2
        self.assertAlmostEqual(result.outgoing[0][0], (2.+abs(s)**2*3.)/denominator)
        self.assertAlmostEqual(result.incident[0][0], (abs(gamma)**2*2.+3.)/denominator)
        self.assertAlmostEqual(result.net_into_device_w_per_hz[0],
                               result.incident[0][0].real-result.outgoing[0][0].real)
        pad = [[0., 0.5], [0.5, 0.]]
        intrinsic = self.library.passive_noise(pad)
        emission = self.library.thermal_boundary_noise([0., 0.], [290., 290.])
        equilibrium = self.library.loaded_noise(pad, intrinsic, [0., 0.], emission)
        kt = 1.380649e-23 * 290.
        self.assertAlmostEqual(equilibrium.outgoing[1][1]/kt, 1.)
        self.assertAlmostEqual(equilibrium.net_into_device_w_per_hz[1]/kt, 0.)
        self.assertEqual(self.library.thermal_boundary_noise([1.], [290.]), ((0j,),))

    def test_loaded_noise_correlations_and_errors(self):
        thru = [[0., 1.], [1., 0.]]
        zero = [[0., 0.], [0., 0.]]
        result = self.library.loaded_noise(thru, zero, [0., 0.], [[2., 1j], [-1j, 2.]])
        self.assertAlmostEqual(result.outgoing[0][1], -1j)
        self.assertAlmostEqual(result.incident[0][1], 1j)
        with self.assertRaises(RFModelError):
            self.library.loaded_noise(thru, zero, [1., 1.], zero)
        with self.assertRaises(ValueError):
            self.library.loaded_noise(thru, [[0.]], [0., 0.], zero)
        with self.assertRaises(RFModelError):
            self.library.loaded_noise(thru, zero, [1.1, 0.], zero)
        with self.assertRaises(RFModelError):
            self.library.thermal_boundary_noise([0.], [-1.])
        with self.assertRaises(ValueError):
            self.library.thermal_boundary_noise([0.], [])

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

    def test_json_signal_feedback_and_power(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/mismatched-signal.json")
        result = analyze(self.library, document)
        for index, phase in enumerate((1., 1j)):
            point = result["samples"][index]
            ports = point["signal"]["ports"]
            self.assertAlmostEqual(complex(*ports[0]["incident"]), phase*8/7)
            self.assertAlmostEqual(complex(*ports[0]["outgoing"]), phase*4/7)
            self.assertAlmostEqual(ports[1]["net_into_device_w"], -48/49)
            self.assertAlmostEqual(sum(port["net_into_device_w"] for port in ports), 0.)
            self.assertLess(point["signal"]["relative_residual"], 1e-12)
            self.assertEqual(point["s"], [[[0., 0.], [1., 0.]], [[1., 0.], [0., 0.]]])
        bad = copy.deepcopy(document)
        bad["signal_boundaries"].pop()
        with self.assertRaises(ValueError):
            analyze(self.library, bad)
        bad = copy.deepcopy(document)
        bad["signal_boundaries"][0]["source"] = 1.
        with self.assertRaises(ValueError):
            analyze(self.library, bad)
        bad = copy.deepcopy(document)
        bad["signal_boundaries"][0]["source_samples"] = [1.]
        with self.assertRaises(ValueError):
            analyze(self.library, bad)

    def test_json_signal_keeps_noise_reference_conditions(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/linear-noise.json")
        original = analyze(self.library, document)
        document["signal_boundaries"] = [
            {"port": ["pad", 0], "source": 1.},
            {"port": ["phase", 1], "reflection": 0.5}]
        result = analyze(self.library, document)
        self.assertEqual(original["samples"][0]["noise_w_per_hz"],
                         result["samples"][0]["noise_w_per_hz"])
        self.assertEqual(len(result["samples"][0]["signal"]["ports"]), 4)

    def test_device_noise_mixed_chain(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/amplifier-noise.json")
        point = analyze(self.library, document)["samples"][0]
        thermal = 1.380649e-23 * 290.
        self.assertAlmostEqual(complex(*point["s"][1][0]), 5.)
        noise = point["noise_w_per_hz"][1][1][0]
        self.assertAlmostEqual(noise / thermal, 25.75)
        # Matched G=100,F=2 amplifier then loss=4: Friis F=2+3/100.
        self.assertAlmostEqual(1. + noise / (thermal * 25.), 2.03)
        original = copy.deepcopy(document)
        del document["devices"][1]["noise"]
        with self.assertRaises(ValueError):
            analyze(self.library, document)
        document = copy.deepcopy(original)
        document["temperature_k"] = 290.
        with self.assertRaises(ValueError):
            analyze(self.library, document)
        for noise_spec in ({"noiseless": False}, {"covariance": [[1.]]}, {},
                           {"temperature_k": 290., "noiseless": True},
                           {"covariance_samples": []}):
            document = copy.deepcopy(original)
            document["devices"][0]["noise"] = noise_spec
            with self.assertRaises(ValueError):
                analyze(self.library, document)

    def test_device_noise_frequency_samples(self):
        document = {"format": "rfmodel.linear-network", "version": 1,
                    "frequencies_hz": [1., 2.], "devices": [
                        {"id": "a", "s": [[0.]], "noise": {"covariance_samples": [[[1.]], [[2.]]]}},
                        {"id": "b", "s": [[0.]], "noise": {"noiseless": True}}],
                    "external_ports": [["b", 0], ["a", 0]]}
        samples = analyze(self.library, document)["samples"]
        self.assertEqual(samples[0]["noise_w_per_hz"][1][1], [1., 0.])
        self.assertEqual(samples[1]["noise_w_per_hz"][1][1], [2., 0.])
        self.assertEqual(samples[1]["noise_w_per_hz"][0][0], [0., 0.])
        document["devices"][0]["noise"] = {"covariance": [[-1.]]}
        with self.assertRaises(RFModelError):
            analyze(self.library, document)

    def test_parameter_models_and_json(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/rlgc-line.json")
        samples = analyze(self.library, document)["samples"]
        self.assertAlmostEqual(complex(*samples[1]["s"][0][0]), 0.6)
        self.assertAlmostEqual(complex(*samples[1]["s"][1][0]), -0.8j)
        self.assertAlmostEqual(complex(*samples[2]["s"][1][0]), -1.)
        document["devices"][0]["model"] = {
            "type": "transmission_line", "characteristic_ohms": 100., "delay_s": 2e-9}
        equivalent = analyze(self.library, document)["samples"]
        for actual, expected in zip(samples, equivalent):
            for row in range(2):
                for column in range(2):
                    self.assertAlmostEqual(complex(*actual["s"][row][column]),
                                           complex(*expected["s"][row][column]))
        dc = self.library.rlgc_line(0., length_m=2., resistance_ohms_per_m=25.)
        self.assertAlmostEqual(dc[1][0], 2. / 3.)
        with self.assertRaises(RFModelError):
            self.library.rlgc_line(1., length_m=-1.)
        document["devices"][0]["model"]["unknown"] = 1
        with self.assertRaises(ValueError):
            analyze(self.library, document)

    def test_amplifier_parameters_and_chain(self):
        scattering = self.library.linear_amplifier(
            1e9, gain_phase_degrees=90., reverse_isolation_db=20., reverse_phase_degrees=-90.,
            input_impedance_ohms=50+50j, output_impedance_ohms=50-50j)
        self.assertAlmostEqual(scattering[0][0], 0.2+0.4j)
        self.assertAlmostEqual(scattering[1][1], 0.2-0.4j)
        self.assertAlmostEqual(scattering[1][0], 10j)
        self.assertAlmostEqual(scattering[0][1], -0.1j)
        with self.assertRaises(RFModelError) as caught:
            self.library.linear_amplifier(1., input_impedance_ohms=-50.)
        self.assertEqual(caught.exception.status, 1)
        document = load(Path(__file__).resolve().parents[1] / "examples/amplifier-chain.json")
        result = analyze(self.library, document)
        for sample in result["samples"]:
            self.assertAlmostEqual(complex(*sample["s"][1][0]), 5j)
            self.assertAlmostEqual(complex(*sample["s"][0][1]), 0.005)
        # Active amplifiers must not acquire fabricated passive thermal noise.
        document["temperature_k"] = 290.
        with self.assertRaises(RFModelError):
            analyze(self.library, document)

    def test_json_complex_amplifier_impedance(self):
        document = {"format": "rfmodel.linear-network", "version": 1,
                    "frequencies_hz": [1e9], "devices": [{"id": "amp", "model": {
                        "type": "linear_amplifier", "gain_db": 0.,
                        "input_impedance_ohms": [50., 50.]}}],
                    "external_ports": [["amp", 0], ["amp", 1]]}
        result = analyze(self.library, document)
        self.assertAlmostEqual(complex(*result["samples"][0]["s"][0][0]), 0.2+0.4j)

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
