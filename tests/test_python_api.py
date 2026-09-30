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

    def test_network_spectrum_internal_feedback_and_dc(self):
        with self.library.network() as network:
            network.add([[0, 0.5], [0.5, 0.2]])
            network.add([[0.3, 0.4], [0.4, 0]])
            network.connect(1, 2)
            result = network.transmit_spectrum(1e6, {7: 2j, 0: 1}, [0, 3])
            self.assertAlmostEqual(result[7], 0.4j / 0.94)
            self.assertAlmostEqual(result[0], 0.2 / 0.94)
            self.assertEqual(network.transmit_spectrum(1e6, {}, [0, 3]), {})
            with self.assertRaises(RFModelError):
                network.transmit_spectrum(1e6, {}, [0, 0])
            with self.assertRaises(ValueError):
                network.transmit_spectrum(1e6, {1: 1}, [0])
        with self.assertRaises(RuntimeError):
            network.transmit_spectrum(1e6, {1: 1}, [0, 3])
        with self.library.network() as network:
            network.add([[0, 1j], [1j, 0]])
            with self.assertRaises(RFModelError):
                network.transmit_spectrum(1e6, {0: 1}, [0, 1])

    def test_json_mixer_linear_network_frequency_evaluation(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/mixer-linear-network.json")
        result = analyze_spectrum(self.library, document)
        spectrum = result["stages"][1]["spectrum"]
        self.assertEqual([entry["bin"] for entry in spectrum], [2, 18])
        for entry in spectrum:
            phase = -2 * math.pi * entry["frequency_hz"] * 31.25e-9
            expected = 0.5 * complex(math.cos(phase), math.sin(phase))
            self.assertAlmostEqual(complex(*entry["amplitude"]), expected)
            self.assertAlmostEqual(entry["power_w"], 0.25)
        document["input"] = []
        self.assertEqual(analyze_spectrum(self.library, document)["stages"][1]["spectrum"], [])

    def test_json_linear_spectrum_rejects_ambiguous_contracts(self):
        original = load(Path(__file__).resolve().parents[1] / "examples/mixer-linear-network.json")
        for field, value in (("frequencies_hz", [1e6]), ("reference_ohms", 75),
                             ("noise_boundaries", []), ("external_ports", [["pad", 0]])):
            document = copy.deepcopy(original)
            document["stages"][1]["network"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                analyze_spectrum(self.library, document)
        for field, value in (("noise", {"noiseless": True}), ("s_samples", [])):
            document = copy.deepcopy(original)
            document["stages"][1]["network"]["devices"][0][field] = value
            with self.assertRaises(ValueError):
                analyze_spectrum(self.library, document)

    def test_touchstone_unicode_snapshot_interpolation_and_lifetime(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "测量器件.s2p"
            path.write_text("# GHz S RI R 75\n1 0 0 .5 0 .1 0 0 0\n"
                            "3 0 0 .25 -.25 .3 .2 0 0\n", encoding="ascii")
            model = self.library.touchstone(path)
            self.assertEqual(model.info, (2, 75., 1e9, 3e9, 0))
            with self.library.touchstone(path, out_of_band="clamp") as clamped:
                self.assertAlmostEqual(clamped.s_parameters(4e9)[1][0], 0.25-0.25j)
                with self.assertRaises(RFModelError):
                    clamped.s_parameters(-1.)
            path.unlink()
            self.assertAlmostEqual(model.s_parameters(2e9)[1][0], 0.375-0.125j)
            self.assertAlmostEqual(model.s_parameters(2e9)[0][1], 0.2+0.1j)
            with self.assertRaises(RFModelError):
                model.s_parameters(4e9)
            with self.assertRaises(RFModelError):
                model.s_parameters(2e9, reference_ohms=0)
            model.close()
            model.close()
            with self.assertRaises(RuntimeError):
                model.s_parameters(2e9)

    def test_touchstone_reference_noise_metadata_and_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matched.s1p"
            path.write_text("# Hz S RI R 75\n1000000 0 0\n", encoding="ascii")
            with self.library.touchstone(path) as model:
                self.assertEqual(model.s_parameters(1e6), ((0j,),))
                self.assertAlmostEqual(model.s_parameters(1e6, reference_ohms=50)[0][0], 0.2)
                reference = weakref.ref(model)
                finalizer = model._finalizer
            del model
            gc.collect()
            self.assertIsNone(reference())
            self.assertFalse(finalizer.alive)
            automatic = self.library.touchstone(path)
            reference = weakref.ref(automatic)
            finalizer = automatic._finalizer
            del automatic
            gc.collect()
            self.assertIsNone(reference())
            self.assertFalse(finalizer.alive)
            with self.assertRaises(ValueError):
                self.library.touchstone(path, out_of_band="extrapolate")
            noise = Path(directory) / "noise.s2p"
            noise.write_text("# GHz S RI R 50\n1 0 0 2 0 0 0 0 0\n1 1 0 0 .1\n",
                             encoding="ascii")
            with self.library.touchstone(noise) as model:
                self.assertEqual(model.info.noise_sample_count, 1)
            path.write_text("[Version] 2.0\n", encoding="ascii")
            with self.assertRaises(RFModelError):
                self.library.touchstone(path)

    def test_json_touchstone_linear_and_converted_frequency_response(self):
        examples = Path(__file__).resolve().parents[1] / "examples"
        document = load(examples / "measured-network.json")
        samples = analyze(self.library, document, base_directory=examples)["samples"]
        for sample, expected in zip(samples, (0.8, 0.5, 0.2)):
            self.assertAlmostEqual(complex(*sample["s"][1][0]), expected)
        document = {"format": "rfmodel.spectrum-chain", "version": 1, "spacing_hz": 1e6,
                    "input": [{"bin": 10, "amplitude": 1}], "stages": [
                        {"id": "lo", "type": "ideal_mixer", "lo_bin": 8},
                        {"id": "data", "type": "linear_network", "network": {
                            "devices": [{"id": "m", "model": {
                                "type": "touchstone", "path": "measured-pad.s2p"}}],
                            "external_ports": [["m", 0], ["m", 1]]}}]}
        spectrum = analyze_spectrum(self.library, document, base_directory=examples)["stages"][1]["spectrum"]
        self.assertEqual([value["bin"] for value in spectrum], [2, 18])
        self.assertAlmostEqual(spectrum[0]["power_w"], 0.64)
        self.assertAlmostEqual(spectrum[1]["power_w"], 0.04)
        document["stages"][0]["lo_bin"] = 9
        with self.assertRaises(RFModelError):
            analyze_spectrum(self.library, document, base_directory=examples)

    def test_touchstone_cli_relative_paths_and_input_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "sample.s1p"
            original = "# Hz S RI R 75\n1000000 0 0\n"
            data.write_text(original, encoding="ascii")
            document = {"format": "rfmodel.linear-network", "version": 1,
                        "frequencies_hz": [1e6], "devices": [{"id": "data", "model": {
                            "type": "touchstone", "path": "sample.s1p"}}],
                        "external_ports": [["data", 0]]}
            model_path = root / "model.json"
            model_path.write_text(json.dumps(document), encoding="utf-8")
            environment = dict(os.environ)
            environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
            command = [sys.executable, "-m", "rfmodel", str(model_path), "--library",
                       str(Path(LIBRARY_PATH).resolve()), "--output"]
            result_path = root / "result.json"
            run = subprocess.run(command + [str(result_path)], env=environment,
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = json.loads(result_path.read_text(encoding="utf-8"))
            self.assertAlmostEqual(result["samples"][0]["s"][0][0][0], 0.2)
            run = subprocess.run(command + [str(data)], env=environment,
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertIn("must not overwrite Touchstone", run.stderr)
            self.assertEqual(data.read_text(encoding="ascii"), original)
            spectrum_document = {
                "format": "rfmodel.spectrum-chain", "version": 1, "spacing_hz": 1e6,
                "input": [{"bin": 1, "amplitude": 1}], "stages": [
                    {"id": "linear", "type": "linear_network", "network": {
                        "devices": document["devices"], "external_ports": [["data", 0], ["data", 1]]}}]}
            model_path.write_text(json.dumps(spectrum_document), encoding="utf-8")
            run = subprocess.run(command + [str(data)], env=environment,
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertIn("must not overwrite Touchstone", run.stderr)
            self.assertEqual(data.read_text(encoding="ascii"), original)

    def test_touchstone_noise_snapshot_reference_and_json(self):
        examples = Path(__file__).resolve().parents[1] / "examples"
        kt = 1.380649e-23 * 290
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "noise.s2p"
            path.write_bytes((examples / "noisy-amplifier.s2p").read_bytes())
            with self.library.touchstone(path) as model:
                path.unlink()
                original = model.noise_correlation(2e9)
                self.assertAlmostEqual(original[0][0] / kt, 1)
                self.assertAlmostEqual(original[1][1] / kt, 4)
                changed = model.noise_correlation(2e9, reference_ohms=50)
                # T=sqrt(.96)*[[1,0],[-.4,1]] for this unilateral network.
                self.assertAlmostEqual(changed[0][0] / kt, .96)
                self.assertAlmostEqual(changed[0][1] / kt, -.384)
                self.assertAlmostEqual(changed[1][1] / kt, 3.9936)
                doubled = model.noise_correlation(2e9, reference_temperature_k=580)
                self.assertAlmostEqual(doubled[1][1] / kt, 8)
                with self.assertRaises(RFModelError):
                    model.noise_correlation(2e9, reference_temperature_k=0)
        document = load(examples / "measured-noise.json")
        for sample in analyze(self.library, document, base_directory=examples)["samples"]:
            for row in range(2):
                for column in range(2):
                    actual = complex(*sample["noise_w_per_hz"][row][column])
                    self.assertAlmostEqual(actual / kt, changed[row][column] / kt)
        for noise in ({"touchstone": False}, {"touchstone": True, "noiseless": True}):
            document["devices"][0]["noise"] = noise
            with self.assertRaises(ValueError):
                analyze(self.library, document, base_directory=examples)

    def test_touchstone_noise_missing_invalid_and_separate_domain(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "noise.s2p"
            network = "# GHz S RI R 50\n1 0 0 2 0 0 0 0 0\n3 0 0 2 0 0 0 0 0\n"
            path.write_text(network, encoding="ascii")
            with self.library.touchstone(path) as model:
                with self.assertRaises(RFModelError):
                    model.noise_correlation(2e9)
            path.write_text(network + "2 3 0 0 0\n", encoding="ascii")
            with self.library.touchstone(path) as model:
                with self.assertRaises(RFModelError):
                    model.noise_correlation(2e9)
            path.write_text(network + "2 3 0 0 1\n", encoding="ascii")
            with self.library.touchstone(path) as model:
                model.s_parameters(1e9)
                with self.assertRaises(RFModelError):
                    model.noise_correlation(1e9)
            with self.library.touchstone(path, out_of_band="clamp") as model:
                self.assertEqual(model.noise_correlation(1e9), model.noise_correlation(2e9))

    def test_json_p1db_fundamental_with_linear_output(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/single-tone-compression.json")
        result = analyze_spectrum(self.library, document)
        first = result["stages"][0]["spectrum"][0]
        last = result["stages"][1]["spectrum"][0]
        self.assertEqual(first["bin"], 1000)
        self.assertAlmostEqual(complex(*first["amplitude"]), .1j)
        self.assertAlmostEqual(first["power_w"] / .01, 1.)
        self.assertAlmostEqual(complex(*last["amplitude"]), .05)
        self.assertAlmostEqual(last["power_w"] / .0025, 1.)
        document["input"].append({"bin": 3, "amplitude": 0})
        self.assertEqual(analyze_spectrum(self.library, document)["stages"], result["stages"])
        document["input"] = []
        self.assertEqual(analyze_spectrum(self.library, document)["stages"][0]["spectrum"], [])

    def test_json_p1db_rejects_multitone_dc_and_overdrive(self):
        original = load(Path(__file__).resolve().parents[1] / "examples/single-tone-compression.json")
        for inputs in ([{"bin": 1, "amplitude": .001}, {"bin": 2, "amplitude": .001}],
                       [{"bin": 0, "amplitude": .001}], [{"bin": 1, "amplitude": 1}]):
            document = copy.deepcopy(original)
            document["input"] = inputs
            with self.subTest(inputs=inputs), self.assertRaises((ValueError, RFModelError)):
                analyze_spectrum(self.library, document)
        original["input"] = []
        original["stages"][0]["power_gain_db"] = 1e308
        with self.assertRaises(RFModelError):
            analyze_spectrum(self.library, original)

    def test_p1db_cli_preserves_output_for_multitone(self):
        original = load(Path(__file__).resolve().parents[1] / "examples/single-tone-compression.json")
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.json"
            output = Path(directory) / "output.json"
            model.write_text(json.dumps(original), encoding="utf-8")
            environment = dict(os.environ)
            environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
            command = [sys.executable, "-m", "rfmodel", str(model), "--library",
                       str(Path(LIBRARY_PATH).resolve()), "--output", str(output)]
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_bytes()
            original["input"].append({"bin": 2, "amplitude": .001})
            model.write_text(json.dumps(original), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertIn("one nonzero RF tone", run.stderr)
            self.assertEqual(output.read_bytes(), saved)

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

    def test_p1db_fundamental_phase_calibration_and_domain(self):
        input_power = 10 ** (-3.9)
        phase = complex(math.cos(.7), math.sin(.7))
        output = self.library.p1db_fundamental(
            math.sqrt(input_power) * phase, power_gain_db=20., output_p1db_dbm=10.)
        self.assertAlmostEqual(abs(output)**2 / .01, 1.)
        self.assertAlmostEqual(output / abs(output), phase)
        self.assertEqual(self.library.p1db_fundamental(
            0, power_gain_db=20., output_p1db_dbm=10.), 0j)
        for incident in (math.sqrt(input_power * 1.01), complex(0, float("nan"))):
            with self.assertRaises(RFModelError) as caught:
                self.library.p1db_fundamental(incident, power_gain_db=20., output_p1db_dbm=10.)
            self.assertEqual(caught.exception.status, 1)
        with self.assertRaises(RFModelError):
            self.library.p1db_fundamental(0., power_gain_db=float("inf"), output_p1db_dbm=10.)

    def test_p1db_total_drive_calibration_and_domain(self):
        total = 10 ** (-3.9)
        incident = complex(0, math.sqrt(total / 4))
        output = self.library.p1db_fundamental(
            incident, power_gain_db=20, output_p1db_dbm=10, total_incident_power_w=total)
        self.assertAlmostEqual(output, .05j)
        self.assertAlmostEqual(self.library.p1db_fundamental(
            incident, power_gain_db=20, output_p1db_dbm=10,
            total_incident_power_w=abs(incident)**2), self.library.p1db_fundamental(
                incident, power_gain_db=20, output_p1db_dbm=10))
        self.assertEqual(self.library.p1db_fundamental(
            0, power_gain_db=20, output_p1db_dbm=10, total_incident_power_w=total), 0j)
        for invalid in (-1, total / 8, total * 1.01, float("inf"), float("nan")):
            with self.subTest(total=invalid), self.assertRaises(RFModelError):
                self.library.p1db_fundamental(
                    incident, power_gain_db=20, output_p1db_dbm=10, total_incident_power_w=invalid)

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

    def test_json_loaded_noise_feedback_and_order(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/loaded-noise.json")
        point = analyze(self.library, document)["samples"][0]
        thermal = 1.380649e-23 * 290
        loaded = point["loaded_noise"]
        # Cold reflecting load: emitted noise is zero; b2 variance is kT,
        # a2 variance is |Gamma|^2 kT, and b1 includes the returned noise.
        for actual, expected in zip(loaded["net_into_device_w_per_hz"],
                                    (0.1875 * thermal, -0.75 * thermal)):
            self.assertAlmostEqual(actual / thermal, expected / thermal)
        self.assertAlmostEqual(loaded["outgoing_w_per_hz"][0][0][0] / thermal, 0.8125)
        self.assertAlmostEqual(loaded["outgoing_w_per_hz"][0][1][0] / thermal, 0.25)
        document["external_ports"].reverse()
        reordered = analyze(self.library, document)["samples"][0]["loaded_noise"]
        self.assertEqual(reordered["net_into_device_w_per_hz"],
                         list(reversed(loaded["net_into_device_w_per_hz"])))
        for row in range(2):
            for column in range(2):
                self.assertEqual(reordered["outgoing_w_per_hz"][row][column],
                                 loaded["outgoing_w_per_hz"][1-row][1-column])

    def test_json_loaded_noise_equilibrium_and_signal_independence(self):
        document = load(Path(__file__).resolve().parents[1] / "examples/loaded-noise.json")
        for boundary in document["noise_boundaries"]:
            boundary.update(reflection=0, temperature_k=290)
        point = analyze(self.library, document)["samples"][0]
        thermal = 1.380649e-23 * 290
        for index in range(2):
            self.assertAlmostEqual(point["loaded_noise"]["outgoing_w_per_hz"][index][index][0]
                                   / thermal, 1)
            self.assertAlmostEqual(point["loaded_noise"]["net_into_device_w_per_hz"][index]
                                   / thermal, 0)
        document["signal_boundaries"] = [
            {"port": ["pad", 0], "reflection": 0.2, "source": 1},
            {"port": ["pad", 1], "reflection": 0.5}]
        with_signal = analyze(self.library, document)["samples"][0]
        for key in ("s", "noise_w_per_hz", "loaded_noise"):
            self.assertEqual(with_signal[key], point[key])

    def test_json_loaded_noise_rejects_incomplete_or_invalid_boundaries(self):
        original = load(Path(__file__).resolve().parents[1] / "examples/loaded-noise.json")
        invalid = [None, [], original["noise_boundaries"][:1],
                   [original["noise_boundaries"][0]] * 2]
        for field, value in (("reflection", 1.1), ("temperature_k", -1),
                             ("port", ["pad", 2]), ("extra", 0)):
            entries = copy.deepcopy(original["noise_boundaries"])
            entries[0][field] = value
            invalid.append(entries)
        entries = copy.deepcopy(original["noise_boundaries"])
        del entries[0]["temperature_k"]
        invalid.append(entries)
        for entries in invalid:
            document = copy.deepcopy(original)
            document["noise_boundaries"] = entries
            with self.subTest(entries=entries), self.assertRaises((ValueError, RFModelError)):
                analyze(self.library, document)
        del original["devices"][0]["noise"]
        with self.assertRaises(ValueError):
            analyze(self.library, original)

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
