"""Independent fundamental-response, feedback and mixed-model ABI regressions."""

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

PARAMETERS = dict(power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=23)
INPUT_ANCHOR = math.sqrt(10 ** (-2.9))


def response(amplitude):
    """Independent scalar curve: total output amplitude and its radial derivative."""
    q = 10 ** (-0.05)
    if amplitude <= INPUT_ANCHOR:
        compression = (1 - q) * (amplitude / INPUT_ANCHOR) ** 2
        return 10 * amplitude * (1 - compression), 10 * (1 - 3 * compression)
    headroom = math.sqrt(10 ** (-0.7)) - math.sqrt(0.1)
    slope = 10 * (3 * q - 2)
    u = slope * (amplitude - INPUT_ANCHOR) / headroom
    return math.sqrt(0.1) + headroom * math.tanh(u), slope / math.cosh(u) ** 2


class AmplifierOperatingPointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def close(self, actual, expected, scale=1, tolerance=5e-8):
        self.assertLessEqual(abs(actual - expected), tolerance * max(scale, abs(expected)))

    def example(self, name="nonlinear-amplifier-feedback"):
        return json.loads((ROOT / "examples" / (name + ".json")).read_text())

    def local(self, waves, channels=None, **options):
        return self.library.linearize_saturating_amplifier(
            1e6, channels or [(0, 1), (1, 1)], waves, **PARAMETERS, **options
        )

    def one_device(self, amplitude=0.02, reflection=0):
        return dict(
            channels=[(0, 1), (1, 1)],
            direct=[[0, 0], [0, 0]],
            source=[amplitude, 0],
            reflection=[0, reflection],
            source_covariance=[[1e-9, 0], [0, 0]],
        )

    def test_complex_derivatives_below_at_and_above_anchor(self):
        channels = [(0, 1), (0, 3), (1, 1), (1, 3)]
        base = [0.8 + 0.1j, 0.2 - 0.5j, 0.2 + 0.1j, -0.1 + 0.2j]
        for output_drive in [False, True]:
            norm = math.sqrt(sum(abs(x) ** 2 for x in base[: 4 if output_drive else 2]))
            for ratio in [0, 0.3, 1, 3]:
                waves = [x * INPUT_ANCHOR * ratio / norm for x in base]
                model = self.local(waves, channels, include_output_drive=output_drive)
                for j in range(4):
                    for direction in [1, 1j]:
                        hi, lo = waves.copy(), waves.copy()
                        hi[j] += 1e-7 * direction
                        lo[j] -= 1e-7 * direction
                        upper = self.local(hi, channels, include_output_drive=output_drive)
                        lower = self.local(lo, channels, include_output_drive=output_drive)
                        for i in range(4):
                            finite = (
                                upper.operating_outgoing[i] - lower.operating_outgoing[i]
                            ) / 2e-7
                            expected = (
                                model.direct[i][j] * direction
                                + model.conjugate[i][j] * complex(direction).conjugate()
                            )
                            self.close(finite, expected, tolerance=1e-5 if ratio == 1 else 5e-8)

    def test_general_affine_offset_reconstructs_nominal(self):
        waves = [0.01 + 0.02j, 0.03j]
        local = self.local(waves, include_output_drive=True)
        self.assertNotAlmostEqual(local.output_offset[1], -local.operating_outgoing[1])
        device = dict(
            channels=[(0, 1), (1, 1)], direct=local.direct, conjugate=local.conjugate, source=waves
        )
        solved = self.library.conversion_network(1e6, [device], output_offset=local.output_offset)
        for actual, expected in zip(solved.outgoing, local.operating_outgoing):
            self.close(actual, expected)

    def test_single_amplifier_python_working_point_and_loaded_noise(self):
        actual = self.library.solve_conversion_operating_point(
            1e6, [self.one_device()], amplifiers=[dict(device=0, **PARAMETERS)], loaded_noise=True
        )
        amplitude, radial = response(0.02)
        gain = amplitude / 0.02
        self.close(actual.waves.outgoing[1], amplitude)
        self.close(actual.waves.noise_covariance[1][1], 0.5 * (gain**2 + radial**2) * 1e-9, 1e-7)
        self.close(actual.waves.noise_complementary[1][1], 0.5 * (radial**2 - gain**2) * 1e-9, 1e-7)
        self.close(
            actual.waves.incident_outgoing_noise_covariance[0][1],
            0.5 * (gain + radial) * 1e-9,
            1e-7,
        )
        self.assertLessEqual(actual.scaled_residual, 1)

    def test_feedback_matches_independent_root_and_radial_tangential_noise(self):
        result = analyze_conversion_network(self.library, self.example())
        low, high = 0.0, 0.2
        for _ in range(80):
            middle = (low + high) / 2
            if middle + 0.2 * response(middle)[0] > 0.2:
                high = middle
            else:
                low = middle
        a = (low + high) / 2
        b, radial = response(a)
        gain = b / a
        self.close(complex(*result["channels"][0]["incident"]), a)
        self.close(complex(*result["channels"][1]["outgoing"]), b)
        radial /= 1 + 0.2 * radial
        gain /= 1 + 0.2 * gain
        self.close(
            complex(*result["noise_covariance_w_per_hz"][1][1]),
            0.5 * (radial**2 + gain**2) * 1e-9,
            1e-8,
        )
        self.close(
            complex(*result["noise_complementary_w_per_hz"][1][1]),
            0.5 * (radial**2 - gain**2) * 1e-9,
            1e-8,
        )

    def test_cross_frequency_covariance_and_complementary(self):
        r = 1.5 * INPUT_ANCHOR
        channels = [(0, 1), (0, 3), (1, 1), (1, 3)]
        device = dict(
            channels=channels,
            direct=[[0] * 4 for _ in range(4)],
            source=[0.8 * r, 0.6 * r, 0, 0],
            source_covariance=[
                [1e-9 if i == j and i < 2 else 0 for j in range(4)] for i in range(4)
            ],
        )
        result = self.library.solve_conversion_operating_point(
            1e6, [device], amplifiers=[dict(device=0, **PARAMETERS)]
        )
        b, radial = response(r)
        gain = b / r
        coupling = 0.5 * (radial**2 - gain**2) * 0.8 * 0.6 * 1e-9
        self.close(result.waves.noise_covariance[2][3], coupling, 1e-7)
        self.close(result.waves.noise_complementary[2][3], coupling, 1e-7)
        self.assertGreater(abs(coupling), 1e-10)

    def test_reflected_output_drive_is_explicit_and_self_consistent(self):
        values = []
        for flag in [False, True]:
            result = self.library.solve_conversion_operating_point(
                1e6,
                [self.one_device(0.03, 0.4)],
                amplifiers=[dict(device=0, include_output_drive=flag, **PARAMETERS)],
            )
            a, b = result.waves.incident[1], result.waves.outgoing[1]
            total = math.sqrt(0.03**2 + (abs(a) ** 2 if flag else 0))
            expected = 0.03 * response(total)[0] / total
            self.close(b, expected)
            self.close(a, 0.4 * b)
            values.append(abs(b))
        self.assertLess(values[1], values[0])

    def test_mixed_mixer_amplifier_network_and_shared_phase_noise(self):
        result = analyze_conversion_network(self.library, self.example("nonlinear-mixer-amplifier"))
        total = math.sqrt(0.5)
        gain = math.sqrt(10 ** (-0.7)) / total
        for b, expected, noise in [
            (2, 0.5 * gain, None),
            (22, 0.5 * gain, None),
            (1, 0, 0),
            (3, 0, 0),
            (21, 0, gain**2 * 1e-10),
            (23, 0, gain**2 * 1e-10),
        ]:
            i, row = next(
                (i, row)
                for i, row in enumerate(result["channels"])
                if (row["device"], row["port"], row["bin"]) == ("amplifier", 1, b)
            )
            self.close(complex(*row["outgoing"]), expected)
            if noise is not None:
                self.close(complex(*result["noise_covariance_w_per_hz"][i][i]), noise, 1e-10)

    def test_reference_noise_figure_uses_converged_derivatives(self):
        doc = dict(
            format="rfmodel.conversion-network",
            version=1,
            spacing_hz=1e6,
            devices=[
                dict(
                    id="amp",
                    channels=[dict(port=p, bin=1) for p in [0, 1]],
                    model=dict(type="saturating_amplifier", **PARAMETERS),
                    noise=dict(noiseless=True),
                )
            ],
            boundaries=[dict(channel=["amp", 0, 1], source=0.02), dict(channel=["amp", 1, 1])],
            operating_point={},
            noise_analyses=[
                dict(
                    name="nf",
                    reference_channels=[["amp", 0, 1]],
                    thermal_channels=[["amp", 0, 1]],
                    output_channel=["amp", 1, 1],
                )
            ],
        )
        metric = analyze_conversion_network(self.library, doc)["noise_analyses"][0]
        b, radial = response(0.02)
        self.close(metric["reference_gain"], 0.5 * ((b / 0.02) ** 2 + radial**2))
        self.close(metric["noise_factor"], 1)

    def test_feedback_warm_start(self):
        doc = self.example()
        first = analyze_conversion_network(self.library, doc)
        doc["operating_point"]["initial_incident"] = [row["incident"] for row in first["channels"]]
        second = analyze_conversion_network(self.library, doc)
        self.assertEqual(second["operating_point"]["iterations"], 0)

    def test_strict_json_model_fields(self):
        for fields in [
            dict(power_gain_db=True),
            dict(output_p1db_dbm="20"),
            dict(input_port=False),
            dict(include_output_drive=1),
            dict(extra=0),
            dict(output_saturation_dbm=20),
        ]:
            with self.subTest(fields=fields):
                doc = self.example()
                doc["devices"][0]["model"].update(fields)
                with self.assertRaises((ValueError, RFModelError)):
                    analyze_conversion_network(self.library, doc)
        for missing in ["operating_point", "output_saturation_dbm"]:
            doc = self.example()
            (doc if missing == "operating_point" else doc["devices"][0]["model"]).pop(missing)
            with self.assertRaises(ValueError):
                analyze_conversion_network(self.library, doc)

    def test_direct_parameters_and_duplicate_model_rejection(self):
        for change in [
            dict(device=True),
            dict(power_gain_db=False),
            dict(input_port=-1),
            dict(include_output_drive=1),
            dict(extra=0),
        ]:
            spec = dict(device=0, **PARAMETERS)
            spec.update(change)
            with self.assertRaises((TypeError, ValueError, RFModelError)):
                self.library.solve_conversion_operating_point(
                    1, [self.one_device()], amplifiers=[spec]
                )
        # Two devices keep the model count valid; the device indices must still be unique.
        with self.assertRaises(RFModelError):
            self.library.solve_conversion_operating_point(
                1,
                [self.one_device(), self.one_device()],
                amplifiers=[dict(device=0, **PARAMETERS)] * 2,
            )
        with self.assertRaises(RFModelError):
            self.library.solve_conversion_operating_point(
                1,
                [self.one_device(), self.one_device()],
                amplifiers=[dict(device=0, **PARAMETERS)],
                mixers=[dict(device=0, lo_reference_amplitude=1)],
            )

    def test_invalid_channels_dc_and_nonfinite_input(self):
        for channels, waves in [
            ([(0, 1), (1, 2)], [0.01, 0]),
            ([(0, 0), (0, 1), (1, 0), (1, 1)], [0.1, 0.01, 0, 0]),
            ([(0, 1), (1, 1)], [math.inf, 0]),
        ]:
            with self.assertRaises(RFModelError):
                self.local(waves, channels)
        result = self.local([0, 0.01, 0, 0], [(0, 0), (0, 1), (1, 0), (1, 1)])
        self.assertEqual(result.operating_outgoing[2], 0)
        self.assertEqual(result.direct[2], (0j,) * 4)

    def test_additional_noise_and_fixed_output_offset(self):
        device = self.one_device()
        result = self.library.solve_conversion_operating_point(
            1e6,
            [device],
            amplifiers=[dict(device=0, **PARAMETERS)],
            output_offset=[0, 0.03j],
            additional_source_covariance=[[2e-9, 0], [0, 0]],
        )
        b, radial = response(0.02)
        self.close(result.waves.outgoing[1], b + 0.03j)
        self.close(
            result.waves.noise_covariance[1][1], 0.5 * ((b / 0.02) ** 2 + radial**2) * 3e-9, 1e-6
        )

    def test_cli_nonconvergence_preserves_previous_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "input.json", Path(folder) / "output.json"
            document = self.example()
            source.write_text(json.dumps(document))
            env = dict(os.environ, PYTHONPATH=str(Path(rfmodel.__file__).parent.parent))
            command = [
                sys.executable,
                "-m",
                "rfmodel",
                str(source),
                "--library",
                LIBRARY_PATH,
                "--output",
                str(output),
            ]
            run = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_bytes()
            document["operating_point"]["max_iterations"] = 1
            source.write_text(json.dumps(document))
            run = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("iteration limit", run.stderr)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()
