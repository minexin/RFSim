"""Channel integration, explicit spectral lines, DC clipping and network measurements."""

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


class ChannelNoiseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_affine_psd_integral_with_clipped_segments(self):
        # n(f)=2+3f has exact integral 1.5*(b*b-a*a)+2*(b-a).
        grid = [0, 0.1, 0.4, 0.7, 1]
        result = self.library.channel_noise(
            [(f, 2 + 3 * f) for f in grid], center_hz=0.5, bandwidth_hz=0.6
        )
        self.assertAlmostEqual(result.noise_power_w, 1.5 * (0.8**2 - 0.2**2) + 2 * 0.6)
        self.assertEqual(result.interpolation_intervals, 3)
        self.assertEqual(result.ratio_state, "no_signal")
        self.assertIsNone(result.carrier_to_noise_db)

    def test_dc_clips_effective_width_including_one_hz_channel(self):
        result = self.library.channel_noise(
            [(0, 4e-21), (1, 4e-21)], center_hz=0, bandwidth_hz=1, desired_lines=[(0, 1)]
        )
        self.assertEqual(result.effective_bandwidth_hz, 0.5)
        self.assertAlmostEqual(result.noise_power_w / 4e-21, 0.5)
        self.assertAlmostEqual(result.mean_noise_density_w_per_hz / 4e-21, 1)
        self.assertEqual(result.desired_line_count, 1)

    def test_tones_are_discrete_and_band_edges_inclusive(self):
        result = self.library.channel_noise(
            [(0, 1), (10, 1)],
            center_hz=5,
            bandwidth_hz=4,
            desired_lines=[(2, 100), (3, 1), (5, 2), (7, 3), (8, 100)],
        )
        self.assertEqual(result.desired_signal_power_w, 6)
        self.assertEqual(result.desired_line_count, 3)
        self.assertAlmostEqual(result.carrier_to_noise_db, 10 * math.log10(6 / 4))

    def test_nonfinite_ratios_are_explicit_states(self):
        for density, power, state in ((0, 1, "noise_free"), (1, 0, "no_signal"), (0, 0, "empty")):
            result = self.library.channel_noise(
                [(0, density), (1, density)],
                center_hz=0.5,
                bandwidth_hz=1,
                desired_lines=[(0.5, power)],
            )
            self.assertEqual(result.ratio_state, state)
            self.assertIsNone(result.carrier_to_noise_db)

    def test_dynamic_range_uses_log_difference(self):
        result = self.library.channel_noise(
            [(0, 1e308), (1, 1e308)], center_hz=0.5, bandwidth_hz=1, desired_lines=[(0.5, 1e-308)]
        )
        self.assertAlmostEqual(result.noise_power_w / 1e308, 1)
        self.assertAlmostEqual(result.carrier_to_noise_db, -6160)
        tiny = float.fromhex("0x0.0000000000001p-1022")
        result = self.library.channel_noise(
            [(0, tiny), (100, tiny)], center_hz=50, bandwidth_hz=100
        )
        self.assertEqual(result.noise_power_w, 100 * tiny)

    def test_grid_refinement_converges_to_quadratic_integral(self):
        errors = []
        for intervals in (2, 4, 8, 16):
            samples = [(i / intervals, 1 + (i / intervals) ** 2) for i in range(intervals + 1)]
            result = self.library.channel_noise(samples, center_hz=0.5, bandwidth_hz=1)
            errors.append(result.noise_power_w - 4 / 3)
        for coarse, fine in zip(errors, errors[1:]):
            self.assertAlmostEqual(coarse / fine, 4)

    def test_conversion_filter_chain_has_correct_noise_and_signal(self):
        document = load(ROOT / "examples/channel-noise-conversion.json")
        result = analyze_conversion_network(self.library, document)
        output, input_measure = result["channel_measurements"]
        epsilon2 = 10 ** (0.1 / 10) - 1

        def rf(f):
            return 1 / (1 + (f / 10) ** 6)

        def if_gain(f):
            x = f / 3
            return 1 / (1 + epsilon2 * (4 * x**3 - 3 * x) ** 2)

        densities = [1e-20 * (rf(10 - k) + rf(10 + k)) * if_gain(k) for k in (1, 2, 3)]
        expected_noise = 1e8 * (0.5 * densities[0] + densities[1] + 0.5 * densities[2])
        expected_signal = rf(12) * if_gain(2)
        self.assertAlmostEqual(output["noise_power_w"] / expected_noise, 1)
        self.assertAlmostEqual(output["desired_signal_power_w"], expected_signal)
        self.assertAlmostEqual(
            output["carrier_to_noise_db"],
            10 * (math.log10(expected_signal) - math.log10(expected_noise)),
        )
        self.assertAlmostEqual(input_measure["noise_power_w"] / 2e-12, 1)
        self.assertEqual(input_measure["desired_signal_power_w"], 1)
        self.assertNotIn("incident_noise_covariance_w_per_hz", result)

    def test_noise_measurement_requires_full_sampling_coverage(self):
        for samples in ([(1, 1), (2, 1)], [(0, 1)], [(0, 1), (0, 2)], [(0, -1), (2, 1)]):
            with self.assertRaises((ValueError, RFModelError)):
                self.library.channel_noise(samples, center_hz=1, bandwidth_hz=2)
        for lines in ([(1, 1), (1, 2)], [(1, -1)], [(math.nan, 1)]):
            with self.assertRaises(RFModelError):
                self.library.channel_noise(
                    [(0, 1), (2, 1)], center_hz=1, bandwidth_hz=2, desired_lines=lines
                )

    def test_invalid_band(self):
        for center, width in (
            (-1, 1),
            (1, 0),
            (1, -1),
            (math.nan, 1),
            (1, math.inf),
            (1e308, 1e-300),
        ):
            with self.assertRaises(RFModelError):
                self.library.channel_noise(
                    [(0, 1), (1e308, 1)], center_hz=center, bandwidth_hz=width
                )

    def test_json_measurements_are_strict(self):
        base = load(ROOT / "examples/channel-noise-conversion.json")
        for change in (
            {"wave": "net"},
            {"port": ["unknown", 0]},
            {"desired_bins": [2, 2]},
            {"desired_bins": [True]},
            {"desired_bins": [100]},
            {"extra": 1},
        ):
            document = copy.deepcopy(base)
            document["channel_measurements"][0].update(change)
            with self.assertRaises(ValueError):
                analyze_conversion_network(self.library, document)
        document = copy.deepcopy(base)
        document["channel_measurements"][1]["name"] = "if_channel"
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.library, document)

    def test_cli_missing_band_coverage_preserves_output(self):
        document = load(ROOT / "examples/channel-noise-conversion.json")
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
            document["channel_measurements"][0]["bandwidth_hz"] = 1e10
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
